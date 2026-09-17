#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "environment.hlsli"

// SSR trace (half res): full-res depth -> half-res reflection layer.
// Output: rgb = reflected HDR x Fresnel x intensity, clamped to
//         kSsrMaxReflected (firefly guard — sun-disc strikes and
//         half-overflow garbage would otherwise spike 10-50x over
//         their neighbors and bloom/meter downstream),
//         a   = edge x distance confidence (0 on a miss).
// Rays that hit nothing contribute nothing: the base HDR already carries
// the analytic sky specular IBL, so mirrors keep their chrome read on a
// miss without double-counting the sky. The bilateral blur + composite
// passes consume (rgb, a); the FULL-res base HDR is only re-added in
// the composite, so it stays sharp.

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords (depth .Load is full-res)
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrTracePush (56 bytes).
struct SsrTracePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint steps;
    float2 invDepthSize; // 1 / full-res depth extent (NOT the pass extent)
    float maxDistance;
    float thickness;
    float intensity;
    float f0;
    uint refineSteps;
    uint frameIndex; // dither epoch: 0 = static legacy pattern (spatial path),
                     // else the record counter (temporal path animates the
                     // ray-origin dither so the TAA pass converges speckle
                     // to its mean instead of parking a static pattern)
};

[[vk::push_constant]]
ConstantBuffer<SsrTracePush> sp;

// Firefly cap: upper bound on the reflected HDR the trace may emit.
// Struck radiance is unbounded in practice (sun disc = full sun lux,
// up to 30000 in the UI, times the intensity knob), while legit
// reflections at defaults top out around the sun disc (~5000). Past the
// cap lives only aliasing spikes (a noisy normal + dithered origin
// striking the disc for a single pixel) and half-overflow garbage —
// exactly what blooms (threshold 1000) and the exposure top bin would
// otherwise amplify. The intensity knob stays the artistic gate below it.
static const float kSsrMaxReflected = 8192.0f;

// Interleaved gradient noise: ray-origin dither against banding. The
// per-frame epoch jumps far across the IGN field (offsets >> 1 px, so
// consecutive frames decorrelate) and wraps every 256 records (bounded
// float precision, ~4 s period at 60 Hz — far past TAA convergence).
float SsrIGN(float2 px, uint frameIndex)
{
    uint f = frameIndex & 255u;
    float2 q = px + float2(float(f) * 17.23f, float(f) * 9.71f);
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

// Full-res pixel-center NDC (framebuffer y-down: top = -1, matching
// ssao.frag.hlsl and forwardplus.cs.hlsl). Correct for the main view
// because proj carries the [1][1] *= -1 Vulkan flip; invProj unprojects
// it exactly, so this stays flip-agnostic like the SSAO raw pass.
float2 SsrFullNdc(int2 fullPx, float2 invDepthSize)
{
    return float2((float(fullPx.x) + 0.5f) * invDepthSize.x * 2.0f - 1.0f,
                  (float(fullPx.y) + 0.5f) * invDepthSize.y * 2.0f - 1.0f);
}

// Facing of a candidate hit: depth-derived normal around the hit UV,
// flipped to face the camera, dotted with the ray direction. Front hits
// (ray strikes the camera-visible side) give <= 0; backface hits (the
// ray arrives from behind the surface) give > 0 — sampling those shows
// through the surface, so the march must reject and continue instead.
// Returns false when the taps straddle background (silhouette): the
// caller keeps the hit rather than deciding on garbage.
bool SsrHitFacing(Texture2D depthTex, float2 uv, float2 fullExtent,
    float2 invDepth, float4x4 invProj, float3 R, out float facing)
{
    facing = 0.0f;
    int2 ex = int2(max(fullExtent - 1.0f, float2(1.0f, 1.0f)));
    float2 cuv = clamp(uv, float2(0.0f, 0.0f), float2(0.9999f, 0.9999f));
    int2 hp = clamp(int2(cuv * fullExtent), int2(0, 0), ex);
    int2 hx = clamp(hp + int2(1, 0), int2(0, 0), ex);
    int2 hy = clamp(hp + int2(0, 1), int2(0, 0), ex);
    float dc = depthTex.Load(int3(hp, 0)).r;
    float dx = depthTex.Load(int3(hx, 0)).r;
    float dy = depthTex.Load(int3(hy, 0)).r;
    if (DofIsBackgroundOrGarbage(dc) || DofIsBackgroundOrGarbage(dx) || DofIsBackgroundOrGarbage(dy))
        return false;
    float3 pc = DofUnprojectView(SsrFullNdc(hp, invDepth), dc, invProj);
    float3 px = DofUnprojectView(SsrFullNdc(hx, invDepth), dx, invProj);
    float3 py = DofUnprojectView(SsrFullNdc(hy, invDepth), dy, invProj);
    float3 Hn = normalize(cross(px - pc, py - pc) + 1e-9f);
    float3 Vh = -pc / max(length(pc), 1e-6f);
    if (dot(Hn, Vh) < 0.0f)
        Hn = -Hn;
    facing = dot(Hn, R);
    return true;
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];

    float2 invDepth = sp.invDepthSize;

    // Half-res pixel -> full-res 2x2 block (same discipline as ssao.frag).
    // Depth .Load needs no sampler (D32 has no linear filtering).
    int2 halfPx = int2(IN.pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;

    // Receiver = nearest valid tap (reverse-Z: max depth = nearest).
    // Never average across a foreground/background discontinuity: that
    // fabricates a center floating in mid-air and smears reflections
    // across silhouettes. All-background (sky/clear/OOB at odd extents)
    // blocks carry no reflector -> miss.
    float dCenter = 0.0f;
    int2 fullPxC = fullBase;
    float dBest = 0.0f;
    if (!DofIsBackgroundOrGarbage(d00) && d00 > dBest) { dBest = d00; dCenter = d00; fullPxC = fullBase; }
    if (!DofIsBackgroundOrGarbage(d10) && d10 > dBest) { dBest = d10; dCenter = d10; fullPxC = fullBase + int2(1, 0); }
    if (!DofIsBackgroundOrGarbage(d01) && d01 > dBest) { dBest = d01; dCenter = d01; fullPxC = fullBase + int2(0, 1); }
    if (!DofIsBackgroundOrGarbage(d11) && d11 > dBest) { dBest = d11; dCenter = d11; fullPxC = fullBase + int2(1, 1); }
    if (DofIsBackgroundOrGarbage(dCenter))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }

    float2 ndcC = SsrFullNdc(fullPxC, invDepth);
    float3 viewPos = DofUnprojectView(ndcC, dCenter, frame.invProj);

    // Depth-derived normal from full-res neighbors one half-step over
    // (same taps as ssao.frag.hlsl). A background neighbor means this
    // block sits on a silhouette: reuse the CENTER depth at the exact
    // neighbor NDC (fronto-parallel assumption). Unprojecting through
    // invProj makes the fallback sign-safe by construction — no
    // hand-rolled view-space offsets that can mirror an axis when the
    // Y-flip changes.
    int2 fullPxX = fullBase + int2(2, 0);
    int2 fullPxY = fullBase + int2(0, 2);
    float dX = depthTex.Load(int3(fullPxX, 0)).r;
    float dY = depthTex.Load(int3(fullPxY, 0)).r;
    float3 posX = DofIsBackgroundOrGarbage(dX)
        ? DofUnprojectView(SsrFullNdc(fullPxX, invDepth), dCenter, frame.invProj)
        : DofUnprojectView(SsrFullNdc(fullPxX, invDepth), dX, frame.invProj);
    float3 posY = DofIsBackgroundOrGarbage(dY)
        ? DofUnprojectView(SsrFullNdc(fullPxY, invDepth), dCenter, frame.invProj)
        : DofUnprojectView(SsrFullNdc(fullPxY, invDepth), dY, frame.invProj);
    float3 N = normalize(cross(posX - viewPos, posY - viewPos) + 1e-9f);
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;
    // Safe for double-sided glass: reflect() is invariant under N -> -N,
    // so the viewer-facing flip above never mirrors the reflection.

    // Fresnel (Schlick, dielectric F0): grazing angles — the bistro
    // shopfront seen from the street — reflect the most.
    float ndv = saturate(dot(N, V));
    float fres = sp.f0 + (1.0f - sp.f0) * pow(1.0f - ndv, 5.0f);

    float3 R = reflect(-V, N);
    // No dot(R, V) early-out: R is mirrored across the viewer-facing N,
    // so dot(R, N) = dot(V, N) > 0 by construction (always above the
    // surface). dot(R, V) < 0 is the COMMON valid case (floor, bistro
    // side glass at grazing: the reflection points away from the camera
    // into the scene). Behind-camera rays are handled in the march
    // (rayDist <= 0 break).

    uint nSteps = clamp(sp.steps, 1, 128);
    float maxStep = max(sp.maxDistance, 0.1f) / float(nSteps);
    float minStep = 0.03f; // 3 cm: strides never crawl below this
    // Dither the ray origin along R to break up step banding; bias 2 mm
    // toward the viewer so thin double-sided panes don't self-hit.
    // Animated per record on the temporal path (sp.frameIndex), static
    // on the spatial path — see SsrTracePush.frameIndex.
    float jitter = SsrIGN(float2(halfPx), sp.frameIndex);
    float3 marchPos = viewPos + R * (maxStep * jitter) + N * 2e-3f;

    float2 hitUv = float2(-1.0f, -1.0f);
    float hitFade = 0.0f;
    float2 fullExtent = float2(1.0f / max(invDepth.x, 1e-9f),
                               1.0f / max(invDepth.y, 1e-9f));
    // Exact bracket start: project + sample the jittered origin instead of
    // assuming it is in front. The jitter can throw the origin behind
    // nearby geometry; an assumed-front bracket would then bisect two
    // behind-points and self-hit the receiver.
    float3 prevPos = marchPos;
    float prevDelta = -1e-3f;
    float2 prevUv = float2(-1.0f, -1.0f);
    float stepLen = minStep; // small -> big: grow into open space, never skip early
    {
        float4 oc = mul(frame.proj, float4(marchPos, 1.0f));
        float2 ouv = (oc.xy / max(oc.w, 1e-9f)) * 0.5f + 0.5f;
        if (ouv.x < 0.0f || ouv.x > 1.0f || ouv.y < 0.0f || ouv.y > 1.0f)
        {
            // Origin already off-screen: the whole march exits. Miss.
            OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return OUT;
        }
        prevUv = ouv;
        int2 opx = int2(min(ouv, float2(0.9999f, 0.9999f)) * fullExtent);
        float od = depthTex.Load(int3(opx, 0)).r;
        if (DofIsBackgroundOrGarbage(od))
            prevDelta = -1e30f; // open space: infinitely in front
        else
        {
            float odist = DofViewDist(od, SsrFullNdc(opx, invDepth), frame.invProj);
            float odelta = -marchPos.z - odist;
            // In-slab origin (2 mm bias against an adjacent surface):
            // treat as front and march on rather than self-hitting.
            prevDelta = (odelta > 0.0f && odelta < max(sp.thickness, 0.01f))
                ? -1e-3f : odelta;
        }
    }
    for (uint i = 0; i < 128; ++i)
    {
        if (i >= nSteps)
            break;
        marchPos += R * stepLen;
        float rayDist = -marchPos.z;
        if (rayDist <= 0.0f)
            break; // behind the camera

        float4 clip = mul(frame.proj, float4(marchPos, 1.0f));
        float invW = 1.0f / max(clip.w, 1e-9f);
        float2 marchNdc = clip.xy * invW;
        // Project to full-res depth UV (NDC y-down -> uv = ndc * 0.5 + 0.5,
        // same as ssao.frag.hlsl line ~148).
        float2 marchUv = marchNdc * 0.5f + 0.5f;
        if (marchUv.x < 0.0f || marchUv.x > 1.0f || marchUv.y < 0.0f || marchUv.y > 1.0f)
            break; // off-screen: miss (no history to stretch)

        // Stride controller: uniform view-space steps leap clean over
        // occluder footprints between samples (at 50 m / 32 steps one
        // stride spans hundreds of pixels for screen-perpendicular rays),
        // so the march tunnels past walls and shows occluded geometry
        // through surfaces. Steer each stride toward ~6 screen pixels:
        // dense where footprints are small, long where the ray drills in
        // depth (few footprints crossed, full reach preserved).
        float strideNext = stepLen;
        {
            float movedPx = length((marchUv - prevUv) * fullExtent);
            float adaptF = clamp(6.0f / max(movedPx, 1e-3f), 0.5f, 2.0f);
            strideNext = clamp(stepLen * adaptF, minStep, maxStep);
        }
        prevUv = marchUv;

        int2 samplePx = int2(marchUv * fullExtent);
        float sampD = depthTex.Load(int3(samplePx, 0)).r;
        if (DofIsBackgroundOrGarbage(sampD))
        {
            // Sky behind the ray: infinitely in front. Keep marching; a
            // later behind-sample bisects back to the silhouette edge.
            prevPos = marchPos;
            prevDelta = -1e30f;
            stepLen = strideNext;
            continue;
        }
        float2 sampNdc = SsrFullNdc(samplePx, invDepth);
        float sceneDist = DofViewDist(sampD, sampNdc, frame.invProj);

        // Hit when the ray has crossed into the surface slab:
        // ray behind the scene depth, within the thickness band.
        float delta = rayDist - sceneDist;
        if (delta > 0.0f && delta < max(sp.thickness, 0.01f))
        {
            // Binary refine: back up half-steps to tighten the hit.
            float3 refinePos = marchPos;
            float refineStep = stepLen * 0.5f;
            for (uint r = 0; r < 8; ++r)
            {
                if (r >= sp.refineSteps)
                    break;
                refinePos -= R * refineStep;
                float4 rc = mul(frame.proj, float4(refinePos, 1.0f));
                float2 rn = (rc.xy / max(rc.w, 1e-9f)) * 0.5f + 0.5f;
                int2 rp = int2(clamp(rn, float2(0.0f, 0.0f), float2(1.0f, 1.0f)) * fullExtent);
                float rd = depthTex.Load(int3(rp, 0)).r;
                if (!DofIsBackgroundOrGarbage(rd))
                {
                    float sd = DofViewDist(rd, SsrFullNdc(rp, invDepth), frame.invProj);
                    if (-refinePos.z > sd)
                        refinePos += R * refineStep; // still inside: go forward
                }
                refineStep *= 0.5f;
            }
            float4 hc = mul(frame.proj, float4(refinePos, 1.0f));
            hitUv = (hc.xy / max(hc.w, 1e-9f)) * 0.5f + 0.5f;
            // Reject backface hits: the ray struck the camera-hidden side,
            // so sampling here would show occluded geometry through the
            // surface. Discard and keep marching instead.
            float facing = 0.0f;
            if (SsrHitFacing(depthTex, hitUv, fullExtent, invDepth, frame.invProj, R, facing)
                && facing > 0.05f)
            {
                hitUv = float2(-1.0f, -1.0f);
                hitFade = 0.0f;
                prevPos = marchPos;
                prevDelta = delta;
                stepLen = strideNext;
                continue;
            }
            // Screen-edge fade: hits near the border smear on motion.
            float2 edge = min(hitUv, 1.0f - hitUv);
            hitFade = saturate(min(edge.x, edge.y) / 0.1f);
            // Distance fade: far hits are exponentially less reliable.
            hitFade *= exp(-rayDist / max(sp.maxDistance, 1e-3f));
            break;
        }
        if (prevDelta < 0.0f && delta >= max(sp.thickness, 0.01f))
        {
            // Front-to-behind transition between samples (a slab the stride
            // stepped clean over): bisect the segment for the entry
            // crossing. Sky/off-screen mids read as front, so
            // silhouette crossings converge to the edge. A few iterations
            // are mandatory here — the unrefined landing point can sit far
            // behind the surface — so the floor is 4 regardless of the
            // refine knob.
            float3 lo = prevPos;  // in front
            float3 hi = marchPos; // behind
            uint nRef = clamp(sp.refineSteps, 4u, 8u);
            for (uint r = 0; r < 8; ++r)
            {
                if (r >= nRef)
                    break;
                float3 mid = (lo + hi) * 0.5f;
                float4 mc = mul(frame.proj, float4(mid, 1.0f));
                float2 mn = (mc.xy / max(mc.w, 1e-9f)) * 0.5f + 0.5f;
                float mDelta = -1e30f; // front until proven behind
                if (mn.x >= 0.0f && mn.x <= 1.0f && mn.y >= 0.0f && mn.y <= 1.0f)
                {
                    int2 mp = int2(min(mn, float2(0.9999f, 0.9999f)) * fullExtent);
                    float mD = depthTex.Load(int3(mp, 0)).r;
                    if (!DofIsBackgroundOrGarbage(mD))
                        mDelta = -mid.z - DofViewDist(mD, SsrFullNdc(mp, invDepth), frame.invProj);
                }
                if (mDelta > 0.0f)
                    hi = mid;
                else
                    lo = mid;
            }
            float4 hc = mul(frame.proj, float4(hi, 1.0f));
            hitUv = (hc.xy / max(hc.w, 1e-9f)) * 0.5f + 0.5f;
            // Same backface reject as the slab path above: a crossing that
            // resolves onto a camera-hidden side would show occluded
            // geometry through the surface. Discard and keep marching.
            float facingX = 0.0f;
            if (SsrHitFacing(depthTex, hitUv, fullExtent, invDepth, frame.invProj, R, facingX)
                && facingX > 0.05f)
            {
                hitUv = float2(-1.0f, -1.0f);
                hitFade = 0.0f;
                prevPos = marchPos;
                prevDelta = delta;
                stepLen = strideNext;
                continue;
            }
            // Screen-edge fade: hits near the border smear on motion.
            float2 edge = min(hitUv, 1.0f - hitUv);
            hitFade = saturate(min(edge.x, edge.y) / 0.1f);
            // Distance fade: far hits are exponentially less reliable.
            hitFade *= exp(-max(-hi.z, 0.0f) / max(sp.maxDistance, 1e-3f));
            break;
        }
        prevPos = marchPos;
        prevDelta = delta;
        stepLen = strideNext;
    }

    if (hitUv.x < 0.0f || hitFade <= 0.0f)
    {
        // Miss: contribute nothing. The base HDR already carries the
        // analytic sky specular IBL, so re-adding sky here double-counts
        // it — a white wash over every miss pixel, patchy under noisy
        // depth-derived normals — while real hits stay faded by hitFade.
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }
    float3 reflected = SanitizeHdr(hdrTex.Sample(smp, hitUv).rgb);
    // Firefly clamp BEFORE the intensity scale leaves the pass: a noisy
    // normal + dithered origin striking the sun disc would otherwise
    // inject up to 4x sun lux next to ~100s neighbors — the spike bloom
    // (threshold 1000) and the exposure top bin then amplify.
    float3 capped = min(reflected * (fres * sp.intensity),
                        float3(kSsrMaxReflected, kSsrMaxReflected, kSsrMaxReflected));
    OUT.outColor = float4(capped, hitFade);
    return OUT;
}
