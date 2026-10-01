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
// The miss case writes nothing (the classify zeroed every non-ray
// pixel's radiance; the miss-silence keeps the analytic sky IBL the
// sole sky contributor — no envmap double-count).

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
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<SsrIntersectPush> sp;


static const float kSsrMaxReflected = 8192.0f; // firefly cap (same as the old trace)

// Receiver G-buffer tap at the half-res ray pixel's full-res tap:
// oct world normal + continuous roughness + metal (see
// gbuffer.frag.hlsl). Unbound G-buffer = the matte sentinel.
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

// Initial ray advance (the SDK's FFX_SSSR_InitialAdvanceRay, verbatim):
// intersect the ray with the half-box pointing away from the origin so
// the march never self-intersects the receiver's own cell.
void SsrInitialAdvanceRay(float3 origin, float3 direction, float3 inv_direction,
    float2 mipRes, float2 mipResInv, float2 floorOff, float2 uvOff,
    out float3 position, out float t)
{
    float2 mipPos = mipRes * origin.xy;
    float2 xyPlane = floor(mipPos) + floorOff;
    xyPlane = xyPlane * mipResInv + uvOff;
    float2 tt = xyPlane * inv_direction.xy - origin.xy * inv_direction.xy;
    t = min(tt.x, tt.y);
    position = origin + t * direction;
}

// One traversal step (the SDK's FFX_SSSR_AdvanceRay, verbatim):
// intersect the ray with the current cell's exit planes (xy cell edge
// + the block's depth plane); the z plane is live only when shooting
// away (reversed-Z: direction.z < 0). Returns whether the tile was
// skipped (crossed without dipping below the block's surface — mip up)
// or stopped at the surface (mip down to refine).
bool SsrAdvanceRay(float3 origin, float3 direction, float3 inv_direction,
    float2 mipPos, float2 mipResInv, float2 floorOff, float2 uvOff,
    float surface_z, inout float3 position, inout float t)
{
    float2 xyPlane = floor(mipPos) + floorOff;
    xyPlane = xyPlane * mipResInv + uvOff;
    float3 boundary = float3(xyPlane, surface_z);
    float3 tt = boundary * inv_direction - origin * inv_direction;
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

    FrameUniforms frame = sp.frameUniforms.Get();
    float2 halfExtent = float2(sp.halfW, sp.halfH);
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
    if (roughness >= sp.glossThreshold)
        return; // stale ray (matte — the classify owns silence)
    bool is_mirror = SsrIsMirrorReflection(roughness);

    float2 uv = (float2(coords) + 0.5f) / max(halfExtent, float2(1.0f, 1.0f));
    float3 viewPos = SsrUnprojectView(SsrFullNdc(fullPxC), dBest, frame.invProj);
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    float3 N = gbuf.normal;
    if (dot(N, V) < 0.0f)
        N = -N;

    // GGX VNDF importance-sampled reflection direction (the SDK's
    // SampleReflectionVector with our IGN epochs replacing blue noise).
    float2 rnd = float2(
        SsrIGN(float2(coords) + float2(11.7f, 5.3f), sp.frameIndex),
        SsrIGN(float2(coords) + float2(7.9f, 19.1f), sp.frameIndex));
    float3 Rview = SsrSampleReflectionVector(V, N, roughness, rnd.x, rnd.y);
    float3 screenOrigin = float3(uv, dBest);
    float3 screenDir = SsrProjectDirection(viewPos, Rview, screenOrigin, frame.proj);
    // Behind-camera rays never hit (the SDK relies on the frustum
    // reject below; guard the degenerate direction explicitly).
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

    float3 invDir;
    if (any(screenDir != float3(0.0f, 0.0f, 0.0f)))
    {
        invDir = float3(1.0f, 1.0f, 1.0f) / screenDir;
    }
    else
    {
        invDir = float3(SSR_FLOAT_MAX, SSR_FLOAT_MAX, SSR_FLOAT_MAX);
    }
    int curMip = is_mirror ? 0 : int(min(sp.mostDetailedMip, uint(max(mipCount - 1, 0))));
    float2 mipRes = float2(hzbW[curMip], hzbH[curMip]);
    float2 mipResInv = float2(1.0f, 1.0f) / max(mipRes, float2(1e-6f, 1e-6f));
    float2 uvOff = 0.005f * exp2(float(int(sp.mostDetailedMip))) / fullExtent;
    uvOff.x = screenDir.x < 0.0f ? -uvOff.x : uvOff.x;
    uvOff.y = screenDir.y < 0.0f ? -uvOff.y : uvOff.y;
    float2 floorOff;
    floorOff.x = screenDir.x < 0.0f ? 0.0f : 1.0f;
    floorOff.y = screenDir.y < 0.0f ? 0.0f : 1.0f;

    float curT;
    float3 pos;
    SsrInitialAdvanceRay(screenOrigin, screenDir, invDir,
        mipRes, mipResInv, floorOff, uvOff, pos, curT);

    bool exitLowOcc = false;
    int travIters = int(min(sp.maxIntersections, 128u));
    int i = 0;
    [loop]
    while (i < travIters && curMip >= int(min(sp.mostDetailedMip, uint(max(mipCount - 1, 0)))) && !exitLowOcc)
    {
        float2 mipPos = mipRes * pos.xy;
        int2 tp = int2(clamp(floor(mipPos), float2(0.0f, 0.0f), mipRes - 1.0f));
        float surfaceZ = sp.ssrHzb.Get().texels[hzbOff[curMip] + tp.y * hzbW[curMip] + tp.x];
        exitLowOcc = !is_mirror && (WaveActiveCountBits(true) <= sp.minOccupancy);
        bool skipped = SsrAdvanceRay(screenOrigin, screenDir, invDir,
            mipPos, mipResInv, floorOff, uvOff, surfaceZ, pos, curT);
        bool outOfRange = skipped && (curMip >= mipCount - 1);
        if (!outOfRange)
        {
            curMip += skipped ? 1 : -1;
            float f = skipped ? 0.5f : 2.0f;
            mipRes *= f;
            mipResInv *= skipped ? 2.0f : 0.5f;
        }
        ++i;
    }
    bool validHit = (i <= travIters);
    float3 hit = pos;

    // ---- Validation (the SDK's FFX_SSSR_ValidateHit, adapted): frustum,
    // self (2px manhattan), background (reverse-Z 0 == sky), the hit
    // backface via the HIT G-buffer normal, the thickness confidence,
    // the border vignette.
    float confidence = 0.0f;
    if (validHit)
    {
        if (hit.x >= 0.0f && hit.y >= 0.0f && hit.x <= 1.0f && hit.y <= 1.0f)
        {
            float2 manhattan = abs(hit.xy - uv);
            if (!(manhattan.x < (2.0f / fullExtent.x) && manhattan.y < (2.0f / fullExtent.y)))
            {
                int2 hitPx = int2(clamp(hit.xy * fullExtent,
                    float2(0.0f, 0.0f), fullExtent - 1.0f));
                float surfZ = depthTex.Load(int3(hitPx, 0)).r;
                if (!DofIsBackgroundOrGarbage(surfZ))
                {
                    SsrGb hg = SsrGbTap(hitPx);
                    // Backface: the ray struck the camera-hidden side.
                    float3 hitViewN = mul((float3x3)frame.view, hg.normal);
                    if (dot(hitViewN, Rview) <= 0.0f)
                    {
                        float3 hitViewPos = SsrUnprojectView(
                            hit.xy * 2.0f - 1.0f, surfZ, frame.invProj);
                        float3 surfViewPos = SsrUnprojectView(
                            hit.xy * 2.0f - 1.0f, hit.z, frame.invProj);
                        float dist = length(surfViewPos - hitViewPos);
                        float conf = 1.0f - saturate(dist / max(sp.thickness, 1e-3f));
                        conf *= conf;
                        float2 fov = 0.05f * float2(fullExtent.y / fullExtent.x, 1.0f);
                        float2 border = smoothstep(float2(0.0f, 0.0f), fov, hit.xy)
                            * (1.0f - smoothstep(float2(1.0f, 1.0f) - fov, float2(1.0f, 1.0f), hit.xy));
                        confidence = conf * border.x * border.y;
                    }
                }
            }
        }
    }

    float3 reflected = float3(0.0f, 0.0f, 0.0f);
    if (confidence > 0.0f)
    {
        Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
        SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
        reflected = hdrTex.Sample(smp, hit.xy).rgb;
        reflected = min(max(reflected, float3(0.0f, 0.0f, 0.0f)), float3(65504.0f, 65504.0f, 65504.0f));
        // Fresnel (Schlick): flat-F0 metals rise to ~1 (colored F0
        // approximated flat — the mask carries scalars only).
        float f0 = lerp(sp.f0, 1.0f, gbuf.metal);
        float ndv = saturate(dot(N, V));
        float fres = f0 + (1.0f - f0) * pow(1.0f - ndv, 5.0f);
        float gloss = 1.0f - roughness;
        reflected = min(reflected * (fres * (gloss * gloss) * sp.intensity),
            float3(kSsrMaxReflected, kSsrMaxReflected, kSsrMaxReflected));
    }

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
