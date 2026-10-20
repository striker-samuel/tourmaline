// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "environment.hlsli"
#include "vertex_pack.hlsli"
#include "hzb.hlsli" // HzbBuffer (the trace Hi-Z BDA block)

// SSR trace (half res): full-res depth + the SSSR-style thin G-buffer ->
// half-res reflection layer.
// Output: rgb = reflected HDR x Fresnel x glossFade x intensity, clamped
//         to kSsrMaxReflected (firefly guard — sun-disc strikes and
//         half-overflow garbage would otherwise spike 10-50x over
//         their neighbors and bloom/meter downstream),
//         a   = edge x distance confidence (0 on a miss).
// Rays that hit nothing contribute nothing: the base HDR already carries
// the analytic sky specular IBL, so mirrors keep their chrome read on a
// miss without double-counting the sky. The bilateral blur + composite
// passes consume (rgb, a); the FULL-res base HDR is only re-added in
// the composite, so it stays sharp.
// Receiver: the nearest valid opaque depth tap (position — reverse-Z max),
// with NORMAL + MATERIAL from the thin G-buffer (RGBA8: oct world normal +
// roughness + metal — see gbuffer.frag.hlsl, written by the Forward+
// depth prepass). The REAL interpolated normal replaces the old
// depth-derived receiver normal (piecewise-planar per triangle = the
// wireframe-on-mirrors artifact source — gone by construction: the
// G-buffer normal is the smooth vertex-interpolated surface normal).
// Unbound G-buffer = legacy mirror (perfect-mirror uniform-F0).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrTracePush (80 bytes — the two
// uint64 BDAs (frame uniforms + the trace Hi-Z) keep every 8-byte member
// aligned; pad0/pad1 land the block on a multiple of 8).
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
    uint gbufferTextureIndex; // thin G-buffer (RGBA8), 0xffffffff = legacy mirror
    uint gbuffer2TextureIndex; // F0-chroma layer 2, 0xffffffff = flat F0 (see ssr_intersect.cs.hlsl)
    vk::BufferPointer<HzbBuffer> ssrHzb; // MAX-reduce trace Hi-Z (nearest-surface), null = linear march
    uint ssrHzbMipCount;                 // mips in the pyramid, 0 = linear march
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<SsrTracePush> sp;

// Firefly cap: upper bound on the reflected HDR the trace may emit.
// Struck radiance is unbounded in practice (sun disc = full sun lux,
// up to 30000 in the UI, times the intensity knob), while legit
// reflections at defaults top out around the sun disc (~5000). Past the
// cap lives only aliasing spikes (a noisy normal + dithered origin
// striking the disc for a single pixel) and half-overflow garbage —
// exactly what blooms (threshold 400) and the exposure top bin would
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

// Thin G-buffer tap (full-res RGBA8 — see gbuffer.frag.hlsl): decodes
// the REAL interpolated world normal (octahedral RG — the smooth
// vertex-interpolated surface normal; the depth-derived receiver
// normal and its per-triangle wireframe artifact are gone by
// construction), continuous roughness (B — drives the (1-r)^2 gloss
// fade + the r^2 cone jitter) and the metal flag (A — F0 rise to ~1).
// Returns the clear sentinel (matte +Z) when unbound — the legacy
// mirror decodes identically through the roughness/metal fields.
struct SsrGbuffer
{
    float3 normal;   // world space, unit
    float roughness; // continuous, 0..1
    float metal;     // 0 or 1
};

SsrGbuffer SsrGbufferTap(int2 fullPx)
{
    SsrGbuffer g;
    g.normal = float3(0.0f, 0.0f, 1.0f);
    g.roughness = 1.0f; // matte sentinel
    g.metal = 0.0f;
    if (sp.gbufferTextureIndex == 0xffffffffu)
    {
        // Legacy (unbound — MSAA / layer off): no receiver data, no
        // reflections. The old depth-derived normal + uniform-F0 mirror
        // fallback is gone with the G-buffer architecture; a +Z mirror
        // would fire garbage reflections, so the honest fallback is the
        // matte sentinel (the early-out below kills the march).
        return g;
    }
    Texture2D gbTex = textures[NonUniformResourceIndex(sp.gbufferTextureIndex)];
    float4 gb = gbTex.Load(int3(fullPx, 0));
    // Oct decode (the inverse of gbuffer.frag.hlsl OctEncodeNormal;
    // same fold/sign as vertex_pack.hlsli OctDecodeNormal).
    float x = gb.x * 2.0f - 1.0f;
    float y = gb.y * 2.0f - 1.0f;
    float z = 1.0f - abs(x) - abs(y);
    if (z < 0.0f)
    {
        float tx = (1.0f - abs(y)) * OctSignH(x);
        float ty = (1.0f - abs(x)) * OctSignH(y);
        x = tx;
        y = ty;
    }
    float l2 = x * x + y * y + z * z;
    g.normal = (l2 > 1e-24f) ? float3(x, y, z) / sqrt(l2) : g.normal;
    g.roughness = gb.z;
    g.metal = (gb.w >= 0.5f) ? 1.0f : 0.0f;
    return g;
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

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];

    float2 invDepth = sp.invDepthSize;

    // Half-res pixel -> full-res 2x2 block (same discipline as ssao.frag).
    // Depth .Load needs no sampler (D32 has no linear filtering).
    int2 halfPx = int2(pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;

    // Receiver = nearest valid opaque tap (reverse-Z: max depth = nearest;
    // never average or fabricate a center across a silhouette).
    float dCenter = 0.0f;
    int2 fullPxC = fullBase;
    {
        float dBest = 0.0f;
        if (!DofIsBackgroundOrGarbage(d00) && d00 > dBest) { dBest = d00; dCenter = d00; fullPxC = fullBase; }
        if (!DofIsBackgroundOrGarbage(d10) && d10 > dBest) { dBest = d10; dCenter = d10; fullPxC = fullBase + int2(1, 0); }
        if (!DofIsBackgroundOrGarbage(d01) && d01 > dBest) { dBest = d01; dCenter = d01; fullPxC = fullBase + int2(0, 1); }
        if (!DofIsBackgroundOrGarbage(d11) && d11 > dBest) { dBest = d11; dCenter = d11; fullPxC = fullBase + int2(1, 1); }
    }
    if (DofIsBackgroundOrGarbage(dCenter))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }

    float2 ndcC = SsrFullNdc(fullPxC, invDepth);
    float3 viewPos = DofUnprojectView(ndcC, dCenter, frame.invProj);

    // Receiver NORMAL + MATERIAL from the thin G-buffer (SSSR input
    // set — see gbuffer.frag.hlsl): the REAL interpolated world normal
    // replaces the depth-derived receiver normal entirely. The depth
    // derivation was piecewise-planar per triangle — on finely
    // tessellated mirror-class receivers (the Bistro Vespa's polished
    // paint) the mirrored image broke along the receiver's OWN
    // triangulation: the sky/lamp reflections caught per-facet and read
    // as the object's wireframe. The G-buffer normal is the smooth
    // vertex-interpolated surface normal — the wireframe source is
    // gone by construction (no ±3 central-difference spans needed).
    SsrGbuffer gbuf = SsrGbufferTap(fullPxC);
    // Matte early-out: roughness >= ~0.94 means the DIELECTRIC fade
    // (1-r)^2 < 1/255 — sub-quantization black. Skips the march for
    // matte dielectrics AND the "no data" clear sentinel (temporal-split
    // frames where the seed pass drew without a G-buffer leg — the
    // sentinel decodes metal 0): wasted march work eliminated, exactly
    // like the old binary gate's silence. Metals keep marching: the
    // fade keeps their energy (see glossFade below) and a near-matte
    // metal's screen-space reflection is its only LOCAL light (the
    // probe's parallax box cannot see the room around it; the SG GI
    // crossover carries no local/dynamic geometry).
    if (gbuf.roughness >= 0.94f && gbuf.metal < 0.5f)
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }
    float3 N = gbuf.normal;
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;
    // Safe for double-sided glass: reflect() is invariant under N -> -N,
    // so the viewer-facing flip above never mirrors the reflection.

    // Receiver material: continuous roughness + metal from the G-buffer.
    // Unbound G-buffer (MSAA / legacy) decodes roughness 0, metal 0 —
    // the uniform-F0 perfect-mirror legacy behavior. Pane note: Blend
    // materials never write the G-buffer (the prepass is opaque-only),
    // so a pane receiver reads the seen-through surface's values — the
    // PaneMask redraw is gone by design, pane reflections ride the
    // diffuse-GI read.
    const float roughness = gbuf.roughness;
    const float matMetal = gbuf.metal;
    const float gloss = 1.0f - roughness;
    // Metal-aware fade (parity with EnvSpecFade, environment.hlsli): the
    // dielectric (1-r)^2 exists because ONE jittered tap + the blur pass
    // cannot integrate a wide GGX lobe — the fade substitutes for the
    // missing spread and dims the speckle. Metals must NOT ride it: the
    // screen-space reflection is their only LOCAL light (the probe's
    // parallax box cannot see the room around them — the sponza banner
    // gold, r~0.5, kept 25% of the knight/floor reflection next to it).
    // Metals keep full energy; the cone jitter + temporal convergence +
    // the roughness-scaled blur radius own the spread. Dielectrics keep
    // the noise discount until the blur carries it alone.
    const float dielectricFade = gloss * gloss; // (1-r)^2: continuous, mirrors at 1
    const float glossFade = lerp(dielectricFade, 1.0f, matMetal);

    // Fresnel (Schlick, PER-CHANNEL F0 — the F0-chroma layer 2; see
    // ssr_intersect.cs.hlsl's twin for the full journey note): the
    // receiver metal tints its reflection by its baseColor chroma.
    // 0xffffffff (the layer absent) = the flat fallback (F0 rises to
    // ~1 for metals — colored F0 approximated flat); dielectrics keep
    // the uniform sp.f0. The texture declares INSIDE the branch (HLSL
    // cannot ternary-assign a Texture2D).
    float ndv = saturate(dot(N, V));
    float3 fres; // per-channel (the tinted branch); the flat branch splats
    if (sp.gbuffer2TextureIndex != 0xffffffffu)
    {
        Texture2D gb2Tex = textures[NonUniformResourceIndex(sp.gbuffer2TextureIndex)];
        float3 chroma = gb2Tex.Load(int3(fullPxC, 0)).rgb;
        float3 f0c = lerp(float3(sp.f0, sp.f0, sp.f0), chroma, matMetal);
        fres = f0c + (1.0f - f0c) * pow(1.0f - ndv, 5.0f);
    }
    else
    {
        float3 f0 = lerp(sp.f0.xxx, float3(1.0f, 1.0f, 1.0f), matMetal);
        fres = f0 + (1.0f - f0) * pow(1.0f - ndv, 5.0f);
    }

    float3 R = reflect(-V, N);
    // Roughness cone jitter: perturb the reflection direction within a
    // cone scaled by roughness^2 (GGX alpha), decorrelated per pixel
    // via IGN. Mirrors (r=0) stay exact; rough surfaces smear the lobe.
    float cone = roughness * roughness;
    if (cone > 0.0f)
    {
        // Two IGN samples for a uniform disc in the tangent plane of R.
        float2 j = float2(
            SsrIGN(float2(halfPx.x + 11.7f, halfPx.y + 5.3f), sp.frameIndex),
            SsrIGN(float2(halfPx.x + 7.9f, halfPx.y + 19.1f), sp.frameIndex)
        ) * 2.0f - 1.0f; // [-1,1]^2
        // Orthonormal basis around R.
        float3 t = normalize(cross(abs(R.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f), R));
        float3 b = cross(R, t);
        float3 Rj = R + (t * j.x + b * j.y) * cone * 1.5f;
        R = normalize(Rj);
    }
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
    // SSSR trace Hi-Z layout (MAX-reduce — see hzb_max.cs.hlsl): the
    // per-mip dims + float offsets mirror gfx_filter.h's HzbMipCount /
    // HzbMipDims / HzbMipOffsetFloats exactly (mip k dims = repeated
    // max((d+1)>>1, 1); offset m = prefix sum of mip sizes; count stops
    // at 1x1, capped 16) so the shader-side indexing agrees with the
    // app-side build byte-for-byte. Precomputed once per half-res pixel;
    // the empty-space skip below indexes it. hzbMips 0 (no pyramid / no
    // BDA) keeps the plain adaptive linear march.
    const uint hzbMips = min(sp.ssrHzbMipCount, 16u);
    uint hzbOff[16]; uint hzbW[16]; uint hzbH[16];
    {
        uint w = uint(fullExtent.x + 0.5f);
        uint h = uint(fullExtent.y + 0.5f);
        uint off = 0;
        [unroll]
        for (uint k = 0; k < 16; ++k) {
            hzbW[k] = w; hzbH[k] = h; hzbOff[k] = off;
            off += w * h;
            w = max((w + 1u) >> 1u, 1u);
            h = max((h + 1u) >> 1u, 1u);
        }
    }
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
        // SSSR Hi-Z empty-space skip: extend the stride through a
        // PROVABLY EMPTY block. The candidate (4x the adaptive stride,
        // capped at maxStep) sweeps a uv rect from the current position
        // to the candidate destination; the mip is the FINEST whose
        // texel covers the rect per axis (texel px size >= the swept
        // rect -> the rect spans at most the current texel + one
        // neighbor per axis), so the 2x2 texel neighborhood around the
        // current texel bounds the whole swept rect. If the
        // neighborhood's NEAREST surface (MAX reversed-Z over the four
        // texels) is at or BEHIND the candidate destination's depth,
        // nothing inside the swept rect lies in the step's depth range
        // — the extended stride cannot step over any surface. Only
        // provably-empty blocks are skipped: a wrong pyramid can miss a
        // reflection, never show one through a wall. Compounds: the
        // extended stride feeds the controller, which re-extends x4
        // again while the space stays empty (up to maxStep).
        if (hzbMips > 0)
        {
            const float candStride = min(strideNext * 4.0f, maxStep);
            if (candStride > strideNext * 1.5f)
            {
                float3 candPos = marchPos + R * candStride;
                if (-candPos.z > 0.0f)
                {
                    float4 cc = mul(frame.proj, float4(candPos, 1.0f));
                    float cw = max(cc.w, 1e-9f);
                    float2 candUv = (cc.xy / cw) * 0.5f + 0.5f;
                    float candDepth = saturate(cc.z / cw); // reversed-Z
                    if (candUv.x >= 0.0f && candUv.x <= 1.0f
                        && candUv.y >= 0.0f && candUv.y <= 1.0f)
                    {
                        float2 dPx = abs(candUv - marchUv) * fullExtent;
                        // Finest mip whose texel (in base px) covers the
                        // swept rect per axis.
                        uint mip = hzbMips - 1u;
                        [loop]
                        for (uint k = 0; k < hzbMips; ++k) {
                            mip = k;
                            float tw = fullExtent.x / float(hzbW[k]);
                            float th = fullExtent.y / float(hzbH[k]);
                            if (dPx.x <= tw && dPx.y <= th)
                                break;
                        }
                        // 2x2 texel neighborhood at mip around the
                        // current uv's texel (clamped at the mip edge).
                        float2 tDim = float2(hzbW[mip], hzbH[mip]);
                        int2 tp = int2(clamp(marchUv * tDim,
                            float2(0.0f, 0.0f), tDim - 1.0f));
                        int2 tx = int2(min(tp.x + 1, int(hzbW[mip]) - 1), tp.y);
                        int2 ty = int2(tp.x, min(tp.y + 1, int(hzbH[mip]) - 1));
                        int2 txy = int2(tx.x, ty.y);
                        const uint bo = hzbOff[mip];
                        const uint bw = hzbW[mip];
                        float n0 = sp.ssrHzb.Get().texels[bo + tp.y * bw + tp.x];
                        float n1 = sp.ssrHzb.Get().texels[bo + tx.y * bw + tx.x];
                        float n2 = sp.ssrHzb.Get().texels[bo + ty.y * bw + ty.x];
                        float n3 = sp.ssrHzb.Get().texels[bo + txy.y * bw + txy.x];
                        float nearest = max(max(n0, n1), max(n2, n3));
                        if (nearest <= candDepth)
                            strideNext = candStride;
                    }
                }
            }
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
    // Gloss fade (metal-aware, see the receiver block above): dielectrics
    // ride the continuous (1-r)^2 — one jittered tap + the blur cannot
    // integrate a wide lobe, the fade is the honest gate for them. Metals
    // keep full energy — their screen-space reflection is the only LOCAL
    // light they get; the roughness-scaled blur radius + the cone jitter
    // + temporal convergence own the spread. Unbound roughness buffer =
    // legacy mirror (1).
    // Firefly clamp BEFORE the intensity scale leaves the pass: a noisy
    // normal + dithered origin striking the sun disc would otherwise
    // inject up to 4x sun lux next to ~100s neighbors — the spike bloom
    // (threshold 400) and the exposure top bin then amplify.
    float3 capped = min(reflected * (fres * glossFade * sp.intensity),
                        float3(kSsrMaxReflected, kSsrMaxReflected, kSsrMaxReflected));
    OUT.outColor = float4(capped, hitFade);
    return OUT;
}
