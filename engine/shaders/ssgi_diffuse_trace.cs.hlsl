// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSGI_DIFFUSE_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"
#include "ssr_compute.hlsli"
#include "gpu_platform.hlsli"

// Diffuse SSGI trace (Engine/Rendergraph/ssgi_diffuse.h): ONE
// cosine-weighted hemisphere ray per half-res pixel per frame, marched
// through the shared final-depth MAX-HZB by the SAME cell-exit mip walk
// the SSR intersect uses (copied here verbatim on purpose — the plan's
// "copy the minimum necessary traversal first, extract to
// screen_space_trace.hlsli once tested"; the two consumers share the
// hzb/receiver conventions but not the sampling model, so the merge
// waits for field validation).
//
// What this pass is NOT: it is not a converted SSR. SSR stays the
// specular-indirect owner (GGX VNDF + Fresnel + the reflection-layer
// composite); this chain adds the DIFFUSE indirect term and owns its
// own targets/history. The output RGB is RAW incoming radiance — NO
// receiver albedo, NO kd, NO Fresnel. The receiver reflectance is
// applied in the FULL-res composition (ssgi_diffuse_compose.frag.hlsl)
// so albedo never leaks across the spatial/temporal filter boundaries
// (the blur would smear a per-receiver tint into its neighbors).
//
// Sampling: cosine-weighted hemisphere around the receiver normal.
//   p(w) = cos(theta) / pi  =>  E[ rho * Li(w) * cos / p ]
//                             = rho * Li(w)
// so the sampled HDR radiance needs NO extra cosine factor — the MC
// estimate of the diffuse integral is just albedo * Li (one ray). The
// hemisphere directions come from the SsrIGN epochs (spatial IGN + the
// per-frame epoch decorrelating records so the temporal pass converges
// the one-ray estimate to the hemisphere mean).
//
// Conventions (identical to ssr_intersect.cs.hlsl — the coordinate
// audit only has to happen once):
//   - framebuffer y-down NDC (top = -1), uv = ndc*0.5+0.5;
//   - reversed-Z (1 = near, 0 = far/sky, clear 0), background
//     d <= 1e-6 via DofIsBackgroundOrGarbage;
//   - the G-buffer oct normal is WORLD space — rotated into view space
//     with (float3x3)frame.view before any hemisphere math;
//   - the trace is VIEW space (SsrUnprojectView / SsrProjectDirection);
//   - the pyramid is the final-depth MAX-HZB (nearest-surface per
//     block), NOT the gfx culling MIN pyramid.
//
// The dispatch is the FULL half-res grid (no SSSR classify/compaction):
// every pixel is a diffuse receiver, so wave compaction has nothing to
// skip — one BDA radiance write + one BDA receiver-metadata write per
// pixel, every frame (both buffers are fully rewritten; no creation
// zero-fill contract, unlike the SSR ray counter's InterlockedAdd).
//
// Rejects (each writes (0,0,0,0) — the confidence-aware blur/temporal
// convention treats a miss as "no signal", never "black light"):
//   background receiver, metallic receiver (metals have ~no diffuse
//   energy — kd is applied at compose, but a metal's ray would be
//   wasted work and its slot must read as a miss), degenerate
//   projection, budget-exhausted traversal (an exhausted walk is an
//   HONEST MISS — never a validated mid-flight position; see the
//   validHit note in the loop), frustum exit, self-intersection (2px
//   manhattan), background hit, backface hit, ray-length budget, and
//   the thickness/border confidence test.

// Must match Engine/Rendergraph/ssgi_diffuse.h SsgiDiffuseTracePush
// (96 bytes). uint64 BDAs lead (scalars never precede a BDA — same
// discipline as every SSR push).
struct SsgiDiffuseTracePush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_WRITE(SsrRadiance) radiance;      // float4 per half-res pixel
    GPU_PTR_READ(SsrReceiverData) receiverData; // packed receiver per half-res pixel
    GPU_PTR_READ(HzbBuffer) hzb;             // the shared final-depth MAX pyramid
    uint hdrTextureIndex;       // the FROZEN pre-compose HDR (radiance source)
    uint depthTextureIndex;     // full-res final depth
    uint gbufferTextureIndex;   // thin G-buffer (oct world normal + rough + metal)
    uint samplerIndex;          // linear clamp (the HDR tap)
    uint halfW;                 // half-res extent (the radiance slot domain)
    uint halfH;
    float invDepthW;            // 1 / full-res depth extent
    float invDepthH;
    float maxDistance;          // ray length budget (m, view units)
    float thickness;            // depth acceptance band (m) -> confidence
    uint frameIndex;            // IGN dither epoch (temporal convergence)
    uint maxIntersections;      // traversal loop cap (exhaustion = miss)
    uint hzbMipCount;
    uint mostDetailedMip;       // the walk's finest level (0: full-res refinement —
                                 // the thickness confidence measures the crossing
                                 // gap against the depth TEXEL, not a 2x2 block)
    uint pad0;
    uint pad1;
};

GPU_PUSH(SsgiDiffuseTracePush, sp);

static const float SSGI_PI = 3.14159265359f;
// Firefly cap (SSR's kSsrMaxReflected twin): GI fireflies are WORSE
// than SSR's — the spatial blur spreads them and the temporal pass can
// park them — so the raw sample clamps at the same proven bound.
static const float kSsgiMaxRadiance = 8192.0f;

// Cosine-weighted hemisphere sample (xi = two uniform randoms): the
// projection disk form — pdf = cos(theta)/pi, the diffuse MC estimator
// needs no post-multiplied cosine (see the header).
float3 SsgiSampleCosineHemisphere(float2 xi)
{
    float phi = 2.0f * SSGI_PI * xi.x;
    float r = sqrt(xi.y);
    return float3(
        r * cos(phi),
        r * sin(phi),
        sqrt(max(0.0f, 1.0f - xi.y)));
}

// Cosine-hemisphere direction around a VIEW-space normal (the local
// +Z axis maps onto N; degenerate-axis guard for near-parallel N).
float3 SsgiSampleDiffuseDirection(float3 normalVS, float2 xi)
{
    float3 N = normalize(normalVS);
    float3 helper = abs(N.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(1.0f, 0.0f, 0.0f);
    float3 T = normalize(cross(helper, N));
    float3 B = cross(N, T);
    float3 local = SsgiSampleCosineHemisphere(xi);
    return normalize(T * local.x + B * local.y + N * local.z);
}

// ---- The Hi-Z traversal (copied VERBATIM from ssr_intersect.cs.hlsl;
// see that file's notes for the NaN-safe boundary forms and the
// reversed-Z above/below semantics. Extraction to a shared
// screen_space_trace.hlsli is deferred until the diffuse hits are
// field-validated — the plan's milestone ordering) ----

float2 SsgiBoundaryT2(float2 boundary, float2 origin, float2 direction)
{
    float2 t;
    t.x = direction.x != 0.0f ? (boundary.x - origin.x) / direction.x : SSR_FLOAT_MAX;
    t.y = direction.y != 0.0f ? (boundary.y - origin.y) / direction.y : SSR_FLOAT_MAX;
    return t;
}

float3 SsgiBoundaryT(float3 boundary, float3 origin, float3 direction)
{
    float3 t;
    t.x = direction.x != 0.0f ? (boundary.x - origin.x) / direction.x : SSR_FLOAT_MAX;
    t.y = direction.y != 0.0f ? (boundary.y - origin.y) / direction.y : SSR_FLOAT_MAX;
    t.z = direction.z != 0.0f ? (boundary.z - origin.z) / direction.z : SSR_FLOAT_MAX;
    return t;
}

void SsgiInitialAdvanceRay(float3 origin, float3 direction,
    float2 mipRes, float2 mipResInv, float2 floorOff, float2 uvOff,
    out float3 position, out float t)
{
    float2 mipPos = mipRes * origin.xy;
    float2 xyPlane = floor(mipPos) + floorOff;
    xyPlane = xyPlane * mipResInv + uvOff;
    float2 tt = SsgiBoundaryT2(xyPlane, origin.xy, direction.xy);
    t = min(tt.x, tt.y);
    position = origin + t * direction;
}

bool SsgiAdvanceRay(float3 origin, float3 direction,
    float2 mipPos, float2 mipResInv, float2 floorOff, float2 uvOff,
    float surface_z, inout float3 position, inout float t)
{
    float2 xyPlane = floor(mipPos) + floorOff;
    xyPlane = xyPlane * mipResInv + uvOff;
    float3 tt = SsgiBoundaryT(float3(xyPlane, surface_z), origin, direction);
    tt.z = direction.z < 0.0f ? tt.z : SSR_FLOAT_MAX;
    float tMin = min(min(tt.x, tt.y), tt.z);
    bool above_surface = surface_z < position.z;
    bool skipped_tile = (asuint(tMin) != asuint(tt.z)) && above_surface;
    t = above_surface ? tMin : t;
    position = origin + t * direction;
    return skipped_tile;
}

[numthreads(8, 8, 1)]
void mainCS(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 coords = dtid.xy;
    if (coords.x >= sp.halfW || coords.y >= sp.halfH)
        return;
    const uint slot = coords.y * sp.halfW + coords.x;

    // Every path below writes the radiance slot exactly once (hit or
    // miss) and the receiver metadata (every in-range pixel — the
    // blur/compose taps reach pixels that never hit, same contract as
    // the SSR classify's receiver writes).
#define SSGI_MISS() { \
        sp.radiance.Get().v[slot] = float4(0.0f, 0.0f, 0.0f, 0.0f); \
        return; }

    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];

    // Receiver = nearest valid opaque tap of the full-res 2x2 block
    // (reverse-Z max — never fabricate a center across a silhouette).
    int2 fullBase = int2(coords) * 2;
    float dBest = 0.0f;
    int2 fullPxC = fullBase;
    {
        float d00 = depthTex.Load(int3(fullBase, 0)).r;
        float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
        float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
        float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;
        if (!DofIsBackgroundOrGarbage(d00) && d00 > dBest) { dBest = d00; fullPxC = fullBase; }
        if (!DofIsBackgroundOrGarbage(d10) && d10 > dBest) { dBest = d10; fullPxC = fullBase + int2(1, 0); }
        if (!DofIsBackgroundOrGarbage(d01) && d01 > dBest) { dBest = d01; fullPxC = fullBase + int2(0, 1); }
        if (!DofIsBackgroundOrGarbage(d11) && d11 > dBest) { dBest = d11; fullPxC = fullBase + int2(1, 1); }
    }

    // The receiver metadata (packed thin-G-buffer sample, RGBA8-unorm
    // semantics — see ssr_receiver.hlsli): the SELECTED receiver's oct
    // world normal + roughness + metal. Background blocks carry the
    // no-data sentinel (decodes +Z / matte / non-metal — the filters'
    // neutral). Metals and misses still pack their receiver: the
    // blur's normal weights need every tap's orientation.
    float4 gbTexel = float4(0.5f, 0.5f, 1.0f, 0.0f); // the sentinel texel
    if (sp.gbufferTextureIndex != 0xffffffffu
        && !DofIsBackgroundOrGarbage(dBest)) {
        Texture2D gbTex = textures[NonUniformResourceIndex(sp.gbufferTextureIndex)];
        gbTexel = gbTex.Load(int3(fullPxC, 0));
    }
    sp.receiverData.Get().v[slot] = DofIsBackgroundOrGarbage(dBest)
        ? kSsrReceiverNoData
        : SsrPackReceiver(gbTexel);

    if (DofIsBackgroundOrGarbage(dBest))
        SSGI_MISS() // sky: no diffuse receiver here

    // Metals: no diffuse energy (kd ~ 0 at compose) — the ray is
    // wasted work and the slot must read as a miss so the composite
    // keeps the metal's probe/specular ambient untouched.
    if (gbTexel.w >= 0.5f)
        SSGI_MISS()

    // The receiver's own full-res pixel is the ray's coordinate system
    // (the two-origins discipline: screenOrigin IS the point
    // viewPos unprojects; coords stay the radiance SLOT domain only).
    float2 invDepthSize = float2(sp.invDepthW, sp.invDepthH);
    float2 fullExtent = float2(1.0f / max(sp.invDepthW, 1e-9f),
                               1.0f / max(sp.invDepthH, 1e-9f));
    float2 receiverUv = (float2(fullPxC) + 0.5f) * invDepthSize;
    float3 viewPos = SsrUnprojectView(SsrFullNdc(fullPxC, invDepthSize),
        dBest, frame.invProj);
    // WORLD oct normal -> VIEW space (the space-fix discipline: the
    // hemisphere must be built around the normal as the camera sees
    // it), viewer-facing flip for double-sided surfaces (reflect-like
    // invariance: sampling around -N would send the hemisphere into
    // the surface).
    float3 N = normalize(mul((float3x3)frame.view, SsrOctDecode(gbTexel.xy)));
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;

    // Cosine-weighted hemisphere direction (IGN epochs, spatially
    // offset per pixel like the SSR VNDF randoms).
    float2 rnd = float2(
        SsrIGN(float2(coords) + float2(11.7f, 5.3f), sp.frameIndex),
        SsrIGN(float2(coords) + float2(7.9f, 19.1f), sp.frameIndex));
    float3 dir = SsgiSampleDiffuseDirection(N, rnd);

    float3 screenOrigin = float3(receiverUv, dBest);
    float3 screenDir = SsrProjectDirection(viewPos, dir, screenOrigin,
        frame.proj, frame.nearPlane);
    // Behind-camera / degenerate projection guard (the diffuse lobe
    // points every which way — grazing receivers fire toward the
    // camera too): reject before the march.
    if (!isfinite(screenDir.x + screenDir.y) || length(screenDir.xy) <= 1e-9f)
        SSGI_MISS()

    // ---- The classic Hi-Z cell-exit mip walk over the shared MAX
    // pyramid (per-mip table mirrors gfx_filter.h byte-for-byte).
    const int mipCount = int(min(sp.hzbMipCount, 16u));
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
    const int finestMip = int(min(sp.mostDetailedMip, uint(max(mipCount - 1, 0))));
    int curMip = finestMip;
    float2 mipRes = float2(hzbW[curMip], hzbH[curMip]);
    float2 mipResInv = 1.0f / max(mipRes, float2(1e-6f, 1e-6f));
    float2 uvOff = 0.005f * exp2(float(finestMip)) / fullExtent;
    uvOff.x = screenDir.x < 0.0f ? -uvOff.x : uvOff.x;
    uvOff.y = screenDir.y < 0.0f ? -uvOff.y : uvOff.y;
    float2 floorOff;
    floorOff.x = screenDir.x < 0.0f ? 0.0f : 1.0f;
    floorOff.y = screenDir.y < 0.0f ? 0.0f : 1.0f;

    float curT;
    float3 pos;
    SsgiInitialAdvanceRay(screenOrigin, screenDir,
        mipRes, mipResInv, floorOff, uvOff, pos, curT);

    // The budget-exhaustion contract (the SSR validHit fix): only the
    // refined-past-finest exit is a hit. An exhausted iteration budget
    // leaves curMip >= finestMip -> miss. (No SSR wave-occupancy
    // early-out here: the full dispatch makes every lane a diffuse
    // ray, so occupancy never drops enough to fire it.)
    int travIters = int(min(sp.maxIntersections, 128u));
    int i = 0;
    [loop]
    while (i < travIters && curMip >= finestMip)
    {
        float2 mipPos = mipRes * pos.xy;
        int2 tp = int2(clamp(floor(mipPos), float2(0.0f, 0.0f), mipRes - 1.0f));
        float surfaceZ = sp.hzb.Get().texels[hzbOff[curMip] + tp.y * hzbW[curMip] + tp.x];
        bool skipped = SsgiAdvanceRay(screenOrigin, screenDir,
            mipPos, mipResInv, floorOff, uvOff, surfaceZ, pos, curT);
        bool outOfRange = skipped && (curMip >= mipCount - 1);
        if (!outOfRange)
        {
            curMip += skipped ? 1 : -1;
            // BOUNDS BEFORE THE TABLE READS (the OOB fix, verbatim).
            if (curMip < finestMip || curMip >= mipCount)
                break;
            mipRes = float2(hzbW[curMip], hzbH[curMip]);
            mipResInv = 1.0f / max(mipRes, float2(1e-6f, 1e-6f));
        }
        ++i;
    }
    const bool validHit = (curMip < finestMip);
    float3 hit = pos;
    if (!validHit)
        SSGI_MISS()

    // ---- Validation (the SSR set, minus the specular-only pieces) ----
    if (!(hit.x >= 0.0f && hit.y >= 0.0f && hit.x <= 1.0f && hit.y <= 1.0f))
        SSGI_MISS()
    // Self-intersection: the 2px manhattan radius around the
    // RECEIVER's own pixel (the ray's actual origin).
    float2 manhattan = abs(hit.xy - receiverUv);
    if (manhattan.x < (2.0f / fullExtent.x) && manhattan.y < (2.0f / fullExtent.y))
        SSGI_MISS()
    int2 hitPx = int2(clamp(hit.xy * fullExtent,
        float2(0.0f, 0.0f), fullExtent - 1.0f));
    float surfZ = depthTex.Load(int3(hitPx, 0)).r;
    if (DofIsBackgroundOrGarbage(surfZ))
        SSGI_MISS()
    // Backface: the ray struck the camera-hidden side (read the HIT's
    // own G-buffer normal, not the receiver's).
    if (sp.gbufferTextureIndex != 0xffffffffu) {
        Texture2D gbTex = textures[NonUniformResourceIndex(sp.gbufferTextureIndex)];
        float4 hgTexel = gbTex.Load(int3(hitPx, 0));
        float3 hitViewN = mul((float3x3)frame.view, SsrOctDecode(hgTexel.xy));
        if (dot(hitViewN, dir) > 0.0f)
            SSGI_MISS()
    }
    float3 hitViewPos = SsrUnprojectView(hit.xy * 2.0f - 1.0f, surfZ, frame.invProj);
    // Ray-length budget (view space; the walk itself has no length
    // notion): the plan's 5 m default keeps the bounce LOCAL.
    if (length(hitViewPos - viewPos) > max(sp.maxDistance, 0.1f))
        SSGI_MISS()
    // Thickness confidence (how far below the hit texel's surface the
    // crossing landed) + the screen-border vignette (off-screen
    // geometry cannot be trusted near the edges — the fallback zone).
    float3 surfViewPos = SsrUnprojectView(hit.xy * 2.0f - 1.0f, hit.z, frame.invProj);
    float dist = length(surfViewPos - hitViewPos);
    float conf = 1.0f - saturate(dist / max(sp.thickness, 1e-3f));
    conf *= conf;
    float2 fov = 0.05f * float2(fullExtent.y / fullExtent.x, 1.0f);
    float2 border = smoothstep(float2(0.0f, 0.0f), fov, hit.xy)
        * (1.0f - smoothstep(float2(1.0f, 1.0f) - fov, float2(1.0f, 1.0f), hit.xy));
    float confidence = conf * border.x * border.y;
    if (confidence <= 0.0f)
        SSGI_MISS()

    // Raw incoming radiance — NO albedo, NO kd, NO Fresnel, NO
    // intensity (all of it is the compose's job). SampleLevel, not
    // Sample: compute shaders have no valid implicit-LOD derivatives.
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    float3 radiance = hdrTex.SampleLevel(smp, hit.xy, 0.0f).rgb;
    radiance = clamp(radiance, float3(0.0f, 0.0f, 0.0f),
        float3(kSsgiMaxRadiance, kSsgiMaxRadiance, kSsgiMaxRadiance));
    sp.radiance.Get().v[slot] = float4(radiance, confidence);
}
