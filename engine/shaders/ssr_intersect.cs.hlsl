// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "environment.hlsli"
#include "vertex_pack.hlsli"
#include "hzb.hlsli"
#include "ssr_compute.hlsli"

// SSR intersect (SSSR intersect port — see FidelityFX-SDK-v1.1.4
// sdk/include/FidelityFX/gpu/sssr/ffx_sssr_intersect.h): indirect
// compute dispatch (8x8 groups; ray_index = group_id.x * 64 +
// group_index, early-out past the frozen ray count). Each thread:
// unpack the ray coords -> receiver G-buffer (normal + roughness) ->
// GGX VNDF importance-sample the reflection direction -> the classic
// Hi-Z traversal (cell-exit mip walk) -> validate the hit (frustum,
// self, background, backface via the HIT G-buffer normal, the
// thickness confidence, the border vignette) -> sample the HDR input
// -> store radiance + the 2x2-quad copies.
//
// Conventions (adapted, NOT verbatim — the SDK assumes D3D y-up NDC +
// normal-Z + a texture mip chain):
//   - uv is y-DOWN (framebuffer == texture rows, like forwardplus.cs):
//     ProjectPosition maps NDC->uv WITHOUT the SDK's y flip.
//   - depth is reversed-Z (1 = near, 0 = far/sky — clear 0): the
//     SDK's FFX_SSSR_OPTION_INVERTED_DEPTH branches are the live ones
//     (above_surface = surface_z < position.z; the z plane is used when
//     direction.z < 0).
//   - the depth hierarchy is the app's MAX-reduce SSR pyramid (hzb_max,
//     nearest-surface per block) in the BDA HzbBuffer with manual mip
//     offsets (mirroring gfx_filter.h's HzbMipCount layout byte-for
//     byte) — NOT a texture chain.
// The miss case: the ray's OWN radiance slot is zeroed at the top of
// mainCS (the early rejects return before any other write — see the
// SLOT OWNERSHIP note there) and every reject path past the traversal
// writes SSR_MISS; the classify zeroes the non-ray pixels (the
// miss-silence keeps the analytic sky IBL the sole sky contributor —
// no envmap double-count).

// Must match Engine/Rendergraph/ssr.h SsrIntersectPush (120 bytes).
// uint64 BDAs lead (see ssr.h — scalars never precede a BDA).
struct SsrIntersectPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<SsrRayList> rayList;
    vk::BufferPointer<SsrRayCounter> rayCounter; // [1] = the frozen count (early-out bound)
    vk::BufferPointer<SsrRadiance> radiance;
    vk::BufferPointer<HzbBuffer> ssrHzb;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint gbufferTextureIndex;
    uint halfW;
    uint halfH;
    float glossThreshold;    // rays exist only for roughness < this (classify gate; re-checked)
    float invDepthW; // 1 / full-res depth width
    float invDepthH;
    float maxDistance;
    float thickness;
    float intensity;
    float f0;
    uint frameIndex; // IGN dither epoch (static on the spatial path)
    uint mostDetailedMip; // rough rays start here (mirror starts at 0)
    uint maxIntersections; // traversal loop cap
    uint minOccupancy;     // wave early-out (non-mirror only)
    uint ssrHzbMipCount;
    uint gbuffer2TextureIndex; // F0-chroma layer 2; 0xffffffff = flat F0
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<SsrIntersectPush> sp;


static const float kSsrMaxReflected = 8192.0f; // firefly cap (same as the old trace)

// Receiver G-buffer tap at the half-res ray pixel's full-res tap:
// oct world normal + continuous roughness + metal (see
// gbuffer.frag.hlsl). Unbound G-buffer = the matte sentinel (the
// default below — the MSAA path pushes UINT32_MAX; the old unguarded
// textures[UINT32_MAX] read was undefined behavior, whatever the
// descriptor array happened to return).
struct SsrGb
{
    float3 normal;
    float roughness;
    float metal;
};

SsrGb SsrGbTap(int2 fullPx)
{
    SsrGb g;
    g.normal = float3(0.0f, 0.0f, 1.0f);
    g.roughness = 1.0f;
    g.metal = 0.0f;
    if (sp.gbufferTextureIndex == 0xffffffffu)
        return g; // MSAA (no G-buffer): the matte sentinel, never a bad fetch
    Texture2D gbTex = textures[NonUniformResourceIndex(sp.gbufferTextureIndex)];
    float4 gb = gbTex.Load(int3(fullPx, 0));
    g.normal = SsrOctDecode(gb.xy);
    g.roughness = gb.z;
    g.metal = (gb.w >= 0.5f) ? 1.0f : 0.0f;
    return g;
}

// Full-res pixel-center NDC (framebuffer y-down: top = -1 — NO y flip,
// unlike the SDK's D3D convention; matches ssao.frag.hlsl).
// Uses the push constant's invDepthW/invDepthH.
float2 SsrFullNdc(int2 fullPx)
{
    return float2((float(fullPx.x) + 0.5f) * sp.invDepthW * 2.0f - 1.0f,
                  (float(fullPx.y) + 0.5f) * sp.invDepthH * 2.0f - 1.0f);
}

// Per-axis boundary crossing parameter t = (boundary - origin) /
// direction, NaN-SAFE for axis-aligned rays: a ZERO direction axis is
// parallel to the plane and never crosses (+MAX). The old
// boundary * inv - origin * inv form divided a zero axis into Inf and
// then Inf - Inf'd into NaN whenever the boundary equaled the origin on
// that axis - exactly the axis-aligned reflections (the vertical
// walls' horizontal mirror rays, the floors' vertical columns).
float2 SsrBoundaryT2(float2 boundary, float2 origin, float2 direction)
{
    float2 t;
    t.x = direction.x != 0.0f ? (boundary.x - origin.x) / direction.x : SSR_FLOAT_MAX;
    t.y = direction.y != 0.0f ? (boundary.y - origin.y) / direction.y : SSR_FLOAT_MAX;
    return t;
}

float3 SsrBoundaryT(float3 boundary, float3 origin, float3 direction)
{
    float3 t;
    t.x = direction.x != 0.0f ? (boundary.x - origin.x) / direction.x : SSR_FLOAT_MAX;
    t.y = direction.y != 0.0f ? (boundary.y - origin.y) / direction.y : SSR_FLOAT_MAX;
    t.z = direction.z != 0.0f ? (boundary.z - origin.z) / direction.z : SSR_FLOAT_MAX;
    return t;
}

// Initial ray advance (the SDK's FFX_SSSR_InitialAdvanceRay, adapted to
// the NaN-safe boundary form): intersect the ray with the half-box
// pointing away from the origin so the march never self-intersects the
// receiver's own cell.
void SsrInitialAdvanceRay(float3 origin, float3 direction,
    float2 mipRes, float2 mipResInv, float2 floorOff, float2 uvOff,
    out float3 position, out float t)
{
    float2 mipPos = mipRes * origin.xy;
    float2 xyPlane = floor(mipPos) + floorOff;
    xyPlane = xyPlane * mipResInv + uvOff;
    float2 tt = SsrBoundaryT2(xyPlane, origin.xy, direction.xy);
    t = min(tt.x, tt.y);
    position = origin + t * direction;
}

// One traversal step (the SDK's FFX_SSSR_AdvanceRay, adapted to the
// NaN-safe boundary form): intersect the ray with the current cell's
// exit planes (xy cell edge + the block's depth plane); the z plane is
// live only when shooting away (reversed-Z: direction.z < 0). Returns
// whether the tile was skipped (crossed without dipping below the
// block's surface - mip up) or stopped at the surface (mip down to
// refine).
// NOTE (the approach-fix revert): arming the z plane for APPROACHING
// rays (direction.z > 0, a receiver's front reflecting the camera-side
// scene) with mirrored untraveled/skip semantics was tried and
// REVERTED: it marched those rays past the surfaces instead of the
// stuck-refine exit, and the stuck position + the thickness validation
// is exactly how the near-side reflections (the floor mirroring the
// knight's nearer parts) were found - the mirrored branch collapsed
// the measured reflections (22.9-luma delta shrunk to 3.0). The
// one-sided z plane stays: approaching rays ride the xy skipping + the
// below-surface stuck-refine, which works.
bool SsrAdvanceRay(float3 origin, float3 direction,
    float2 mipPos, float2 mipResInv, float2 floorOff, float2 uvOff,
    float surface_z, inout float3 position, inout float t)
{
    float2 xyPlane = floor(mipPos) + floorOff;
    xyPlane = xyPlane * mipResInv + uvOff;
    float3 tt = SsrBoundaryT(float3(xyPlane, surface_z), origin, direction);
    tt.z = direction.z < 0.0f ? tt.z : SSR_FLOAT_MAX;
    float tMin = min(min(tt.x, tt.y), tt.z);
    // Reversed-Z: larger z = nearer. The ray is above (in front of) the
    // block's nearest surface while position.z exceeds surface_z.
    bool above_surface = surface_z < position.z;
    // Bitwise-equality test (the SDK's NaN-safe form): the min came from
    // an xy edge (not the z clamp) and we stayed above the surface.
    bool skipped_tile = (asuint(tMin) != asuint(tt.z)) && above_surface;
    t = above_surface ? tMin : t;
    position = origin + t * direction;
    return skipped_tile;
}

[numthreads(8, 8, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint ray_index = groupId.x * 64u + groupIndex;
    uint ray_count = sp.rayCounter.Get().count[1];
    if (ray_index >= ray_count)
        return;

    uint packed = sp.rayList.Get().rays[ray_index];
    uint2 coords;
    bool copyH, copyV, copyD;
    SsrUnpackRayCoords(packed, coords, copyH, copyV, copyD);

    // SLOT OWNERSHIP, part 1 (the stale-radiance fix): the classify
    // zeroes only NON-ray pixels; this ray's OWN slot is the
    // intersect's to write — and the early rejects below (the stale
    // ray, the matte re-check, the degenerate direction) return before
    // the SSR_MISS macro is even defined, so they used to leave the
    // slot holding LAST FRAME's radiance in the reused buffer (moving
    // cuts parked ghosts). Zero it IMMEDIATELY after the unpack —
    // every later path overwrites it (the hit sample or SSR_MISS),
    // so the early rejects fall through safe. The quad-COPY siblings
    // stay the late writes' job (only the final sample knows the copy
    // semantics — and the copy flags are packed false today:
    // is_base_ray = true).
    sp.radiance.Get().v[coords.y * sp.halfW + coords.x] =
        float4(0.0f, 0.0f, 0.0f, 0.0f);

    FrameUniforms frame = sp.frameUniforms.Get();
    float2 fullExtent = float2(1.0f / max(sp.invDepthW, 1e-9f),
                               1.0f / max(sp.invDepthH, 1e-9f));

    // Receiver = nearest valid opaque tap of the full-res 2x2 block
    // (reverse-Z max — never fabricate a center across a silhouette).
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
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
    if (DofIsBackgroundOrGarbage(dBest))
        return; // stale ray (the classify validated; keep the guard)

    SsrGb gbuf = SsrGbTap(fullPxC);
    float roughness = gbuf.roughness;
    // Matte re-check (the classify owns the gate — this guards stale
    // rays): matte DIELECTRICS die (the dielectric fade would be
    // sub-quantization anyway). Metals keep marching at any roughness —
    // their screen-space reflection is their only LOCAL light (see the
    // fade at the sample below); the no-data sentinel decodes metal 0
    // so it still dies here.
    if (roughness >= sp.glossThreshold && gbuf.metal < 0.5f)
        return;
    bool is_mirror = SsrIsMirrorReflection(roughness);

    // RAY-ORIGIN CONSISTENCY (the two-origins fix): viewPos
    // unprojects the SELECTED receiver (fullPxC + dBest — the block's
    // nearest valid texel), but the screen-space ray used to start
    // from the half-res pixel CENTER (coords + 0.5) — a different
    // point, up to half a full-res texel away per axis, and across a
    // depth edge a DIFFERENT surface's pixel. The march then treated
    // screenOrigin and viewPos as the same projected ray. One
    // coordinate system now: the screen origin IS the receiver's own
    // full-res pixel (the exact point viewPos unprojects; the
    // self-intersection check below compares against it too). coords
    // remain the radiance SLOT domain only.
    float2 receiverUv = (float2(fullPxC) + 0.5f)
        * float2(sp.invDepthW, sp.invDepthH);
    float3 viewPos = SsrUnprojectView(SsrFullNdc(fullPxC), dBest, frame.invProj);
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    // SPACE FIX: the G-buffer oct normal is WORLD space (gbuffer.frag
    // encodes the VS's world normal), but V/viewPos/Rview/the screen
    // projection below are VIEW space — reflecting V against the world
    // normal rotated every ray by the view rotation: reflections fired
    // at a fixed wrong angle (the ~45-deg right-biased streaks). Rotate
    // the decoded normal into view space first (frame.view = world->view).
    float3 N = normalize(mul((float3x3)frame.view, gbuf.normal));
    if (dot(N, V) < 0.0f)
        N = -N;

    // GGX VNDF importance-sampled reflection direction (the SDK's
    // SampleReflectionVector with our IGN epochs replacing blue noise).
    float2 rnd = float2(
        SsrIGN(float2(coords) + float2(11.7f, 5.3f), sp.frameIndex),
        SsrIGN(float2(coords) + float2(7.9f, 19.1f), sp.frameIndex));
    // SIGN (the NADA hunt's second half, with the sampler-body fix in
    // ssr_compute.hlsli - both notes tell the full journey): the
    // sampler takes the CAMERA-TO-SURFACE incident ray (it negates
    // internally into the Heitz Ve; see SsrSampleReflectionVector's
    // contract). The old call passed V (surface-to-camera) - the
    // opposite hemisphere, the outgoing direction mirrored toward the
    // camera where the marcher's z plane is dead. V stays
    // surface-to-camera below (the Fresnel ndv).
    float3 Rview = SsrSampleReflectionVector(
        normalize(viewPos), N, roughness, rnd.x, rnd.y);
    float3 screenOrigin = float3(receiverUv, dBest);
    float3 screenDir = SsrProjectDirection(viewPos, Rview, screenOrigin, frame.proj);
    // Behind-camera rays never hit (the SDK relies on the frustum
    // reject below; guard the degenerate direction explicitly). Safe
    // as a bare return: the slot was zeroed at the top.
    if (length(screenDir.xy) <= 1e-9f)
        return;

    // ---- The classic Hi-Z traversal (the SDK's
    // FFX_SSSR_HierarchicalRaymarch, adapted): cell-exit mip walk over
    // the app's MAX-reduce SSR pyramid (nearest-surface per block —
    // the ray-trace direction, unlike the culling MIN pyramid).
    int mipCount = int(min(sp.ssrHzbMipCount, 16u));
    // Per-mip (dims, offset) table (mirrors gfx_filter.h's HzbMipDims +
    // HzbMipOffsetFloats byte-for-byte: repeated max((d+1)>>1, 1),
    // prefix-sum offsets).
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

    // Per-ray finest mip (MIRROR FIX): the old init started mirror rays
    // (roughness < 0.0001) at mip 0 while the loop condition demanded
    // curMip >= sp.mostDetailedMip (1) - mirrors NEVER entered the
    // traversal, fell straight through with pos = the origin, and the
    // validation's 2px self-reject killed them. The SDK's intent: the
    // mirror's finest mip IS 0. One definition, used by the init, the
    // loop condition, and the validHit test below.
    const int finestMip = is_mirror
        ? 0
        : int(min(sp.mostDetailedMip, uint(max(mipCount - 1, 0))));
    int curMip = finestMip;
    float2 mipRes = float2(hzbW[curMip], hzbH[curMip]);
    float2 mipResInv = 1.0f / max(mipRes, float2(1e-6f, 1e-6f));
    float2 uvOff = 0.005f * exp2(float(int(sp.mostDetailedMip))) / fullExtent;
    uvOff.x = screenDir.x < 0.0f ? -uvOff.x : uvOff.x;
    uvOff.y = screenDir.y < 0.0f ? -uvOff.y : uvOff.y;
    float2 floorOff;
    floorOff.x = screenDir.x < 0.0f ? 0.0f : 1.0f;
    floorOff.y = screenDir.y < 0.0f ? 0.0f : 1.0f;

    float curT;
    float3 pos;
    SsrInitialAdvanceRay(screenOrigin, screenDir,
        mipRes, mipResInv, floorOff, uvOff, pos, curT);

    bool exitLowOcc = false;
    int travIters = int(min(sp.maxIntersections, 128u));
    int i = 0;
    [loop]
    while (i < travIters && curMip >= finestMip && !exitLowOcc)
    {
        float2 mipPos = mipRes * pos.xy;
        int2 tp = int2(clamp(floor(mipPos), float2(0.0f, 0.0f), mipRes - 1.0f));
        float surfaceZ = sp.ssrHzb.Get().texels[hzbOff[curMip] + tp.y * hzbW[curMip] + tp.x];
        exitLowOcc = !is_mirror && (WaveActiveCountBits(true) <= sp.minOccupancy);
        bool skipped = SsrAdvanceRay(screenOrigin, screenDir,
            mipPos, mipResInv, floorOff, uvOff, surfaceZ, pos, curT);
        bool outOfRange = skipped && (curMip >= mipCount - 1);
        if (!outOfRange)
        {
            curMip += skipped ? 1 : -1;
            // BOUNDS BEFORE THE TABLE READS (the OOB fix): a successful
            // refine past the finest mip is the traversal's HIT exit —
            // mirror rays (finestMip 0) drove curMip to -1 and the reads
            // below touched hzbW[-1] before the while condition caught
            // it (dead values, the compiler may drop them, but the
            // source read was out of bounds). Leaving the TOP of the
            // pyramid is the miss-side belt (the outOfRange guard above
            // normally catches that first). validHit below keeps its
            // meaning: ONLY the refined-past-finest exit is a hit; the
            // budget-exhaustion, occupancy, and out-of-pyramid exits
            // stay misses.
            if (curMip < finestMip || curMip >= mipCount)
                break;
            // EXACT per-mip resolution (the old *0.5/2.0 drift): the HZB
            // halves with ceiling ((w+1)>>1 - a 15-wide mip becomes 8,
            // not 7.5), so the traversal's hand-scaled mipRes silently
            // disagreed with the packed pyramid at every odd dimension
            // and the walk read the wrong cells at coarse mips. Read
            // the table instead.
            mipRes = float2(hzbW[curMip], hzbH[curMip]);
            mipResInv = 1.0f / max(mipRes, float2(1e-6f, 1e-6f));
        }
        ++i;
    }
    // validHit (the budget-exhaustion fix): the loop exits three ways -
    // refined past the finest mip (a crossing candidate: the ONLY hit
    // exit), the iteration budget out, or the wave-occupancy early-out.
    // The old (i <= travIters) was TRUE in all three (a budget-exhausted
    // ray validated a mid-flight position as a "hit"); the SDK has the
    // same defect. Only the refined exit is a hit; the occupancy exit's
    // position is mid-flight by construction.
    bool validHit = (curMip < finestMip) && !exitLowOcc;
    float3 hit = pos;

    // ---- Validation (the SDK's FFX_SSSR_ValidateHit, adapted): frustum,
    // self (2px manhattan), background (reverse-Z 0 == sky), the hit
    // backface via the HIT G-buffer normal, the ray-length budget, the
    // thickness confidence, the border vignette - early-returns, one
    // REJECT write per level.
    // SLOT OWNERSHIP (the stale-radiance fix): the classify zeroes only
    // NON-ray pixels; a ray's own slot (and its quad-copy siblings) is
    // the intersect's to write. The old fall-through form always wrote
    // the final sample (radiance or (0,0,0,0)) - the early-return
    // restructure initially skipped the write on rejects and the slots
    // parked LAST FRAME's radiance (temporal ghosts). Every reject path
    // writes the miss (slot + copies) before returning.
    const uint missIdx = coords.y * sp.halfW + coords.x;
    const uint missHoriz = (coords.y * sp.halfW) + (coords.x ^ 1u);
    const uint missVert = ((coords.y ^ 1u) * sp.halfW) + coords.x;
    const uint missDiag = ((coords.y ^ 1u) * sp.halfW) + (coords.x ^ 1u);
#define SSR_MISS() { \
        float4 miss_ = float4(0.0f, 0.0f, 0.0f, 0.0f); \
        sp.radiance.Get().v[missIdx] = miss_; \
        if (copyH) sp.radiance.Get().v[missHoriz] = miss_; \
        if (copyV) sp.radiance.Get().v[missVert] = miss_; \
        if (copyD) sp.radiance.Get().v[missDiag] = miss_; \
        return; }
    if (!validHit)
        SSR_MISS()
    if (!(hit.x >= 0.0f && hit.y >= 0.0f && hit.x <= 1.0f && hit.y <= 1.0f))
        SSR_MISS()
    // Self-intersection: the 2px manhattan radius is measured from the
    // RECEIVER's own pixel (the ray's actual origin — the two-origins
    // fix; the old half-res block center could sit a texel away from
    // the receiver and shrink/grow the reject window around the wrong
    // point).
    float2 manhattan = abs(hit.xy - receiverUv);
    if (manhattan.x < (2.0f / fullExtent.x) && manhattan.y < (2.0f / fullExtent.y))
        SSR_MISS()
    int2 hitPx = int2(clamp(hit.xy * fullExtent,
        float2(0.0f, 0.0f), fullExtent - 1.0f));
    float surfZ = depthTex.Load(int3(hitPx, 0)).r;
    if (DofIsBackgroundOrGarbage(surfZ))
        SSR_MISS()
    SsrGb hg = SsrGbTap(hitPx);
    // Backface: the ray struck the camera-hidden side.
    float3 hitViewN = mul((float3x3)frame.view, hg.normal);
    if (dot(hitViewN, Rview) > 0.0f)
        SSR_MISS()
    float3 hitViewPos = SsrUnprojectView(
        hit.xy * 2.0f - 1.0f, surfZ, frame.invProj);
    // Ray-length budget: the screen-space walk has no world-length
    // notion - sp.maxDistance was declared and never read. Reject
    // crossings whose view-space travel (the ray is straight in view
    // space, so the chord equals the path) exceeds the budget.
    if (length(hitViewPos - viewPos) > max(sp.maxDistance, 0.1f))
        SSR_MISS()
    float3 surfViewPos = SsrUnprojectView(
        hit.xy * 2.0f - 1.0f, hit.z, frame.invProj);
    float dist = length(surfViewPos - hitViewPos);
    float conf = 1.0f - saturate(dist / max(sp.thickness, 1e-3f));
    conf *= conf;
    float2 fov = 0.05f * float2(fullExtent.y / fullExtent.x, 1.0f);
    float2 border = smoothstep(float2(0.0f, 0.0f), fov, hit.xy)
        * (1.0f - smoothstep(float2(1.0f, 1.0f) - fov, float2(1.0f, 1.0f), hit.xy));
    float confidence = conf * border.x * border.y;
    if (confidence <= 0.0f)
        SSR_MISS()

    // All rejects early-returned above: confidence > 0 is guaranteed
    // here (the old confidence-0 fallthrough branch is gone with the
    // restructure).
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    // SampleLevel, not Sample: implicit-LOD derivatives are invalid in a
    // compute shader (divergent ray dispatches make them meaningless
    // anyway); the reflection reads mip 0 explicitly.
    float3 reflected = hdrTex.SampleLevel(smp, hit.xy, 0.0f).rgb;
    reflected = min(max(reflected, float3(0.0f, 0.0f, 0.0f)), float3(65504.0f, 65504.0f, 65504.0f));
    // Fresnel (Schlick, PER-CHANNEL F0 — the F0-chroma layer 2): the
    // receiver metal tints its reflection by its OWN baseColor chroma
    // (the gold brocade's reflections come out gold, not the
    // white-mirror flat F0=1 the scalar mask forced). f0c = the layer
    // 2's RGB (baseColorFactor x texel) for metals, the dielectric
    // scalar for dielectrics; the Schlick runs per channel (each
    // channel's specular rise stays its own color — colored metals
    // keep colored reflections at grazing angles too).
    // 0xffffffff (the layer absent — no G-buffer leg / the optional
    // target unbound) = the flat fallback, today's behavior. The
    // texture declares INSIDE the branch (HLSL cannot ternary-assign
    // a Texture2D — the normalTex pattern).
    float ndv = saturate(dot(N, V));
    float3 fres;
    const bool gbuffer2On = sp.gbuffer2TextureIndex != 0xffffffffu;
    if (gbuffer2On)
    {
        Texture2D gb2Tex = textures[NonUniformResourceIndex(sp.gbuffer2TextureIndex)];
        // The RECEIVER's chroma at the SAME full-res pixel the
        // receiver G-buffer tap chose (fullPxC — the 2x2 block's
        // nearest valid tap; never a fabricated center).
        float3 chroma = gb2Tex.Load(int3(fullPxC, 0)).rgb;
        float3 f0c = lerp(float3(sp.f0, sp.f0, sp.f0), chroma, gbuf.metal);
        fres = f0c + (1.0f - f0c) * pow(1.0f - ndv, 5.0f);
    }
    else
    {
        float f0 = lerp(sp.f0, 1.0f, gbuf.metal);
        fres = f0 + (1.0f - f0) * pow(1.0f - ndv, 5.0f);
    }
    // Metal-aware fade (parity with ssr.frag.hlsl and
    // environment.hlsli's EnvSpecFade): the dielectric (1-r)^2
    // exists because ONE VNDF-sampled ray + the blur pass cannot
    // integrate a wide GGX lobe — the fade substitutes for the
    // missing spread and dims the speckle. Metals keep full energy:
    // the screen-space reflection is their only LOCAL light (the
    // probe's parallax box cannot see the room around them — the
    // banner gold, r~0.5, kept 25% of the knight/floor reflection
    // next to it). The roughness-scaled blur radius + the VNDF cone
    // + temporal convergence own the spread.
    float gloss = 1.0f - roughness;
    const float dielectricFade = gloss * gloss;
    const float specFade = lerp(dielectricFade, 1.0f, gbuf.metal);
    reflected = min(reflected * (fres * specFade * sp.intensity),
        float3(kSsrMaxReflected, kSsrMaxReflected, kSsrMaxReflected));
    float4 outSample = float4(reflected, confidence);
    uint flatIdx = coords.y * sp.halfW + coords.x;
    sp.radiance.Get().v[flatIdx] = outSample;

    // 2x2-quad copies (the SDK's copy scheme): the packed flags name the
    // siblings that share this ray's radiance.
    uint2 copyTarget = coords ^ 1u;
    if (copyH)
    {
        uint2 cc = uint2(copyTarget.x, coords.y);
        sp.radiance.Get().v[cc.y * sp.halfW + cc.x] = outSample;
    }
    if (copyV)
    {
        uint2 cc = uint2(coords.x, copyTarget.y);
        sp.radiance.Get().v[cc.y * sp.halfW + cc.x] = outSample;
    }
    if (copyD)
    {
        sp.radiance.Get().v[copyTarget.y * sp.halfW + copyTarget.x] = outSample;
    }
}

