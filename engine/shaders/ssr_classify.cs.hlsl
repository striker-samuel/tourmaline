#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "ssr_compute.hlsli"

// SSR classify (SSSR classify-tiles port — see FidelityFX-SDK-v1.1.4
// sdk/include/FidelityFX/gpu/sssr/ffx_sssr_classify_tiles.h): full
// half-res dispatch (8x8 groups); per pixel decides whether a reflection
// ray is wanted and WAVE-COMPACTS the wanted rays into the ray list.
//
// Background/matte pixels (beyond the gloss threshold) write radiance
// (0,0,0,0) directly — every half-res pixel is owned by exactly one
// of {classify (silence), intersect (ray)} per frame, so the radiance
// buffer is complete with no clear and no stale. The denoiser-tile
// list is skipped (the bilateral blur is fullscreen anyway); the
// env-map fallback is skipped (a miss contributes nothing — the base
// HDR carries the sky); variance-guided tracing is skipped (no
// variance history).
//
// Outputs:
//   ray list  : packed uint per ray (coords + copy flags — see
//               UnpackRayCoords below; the intersect unpacks them)
//   counter   : the ray count (read + reset by the indirect-args pass)
//   radiance  : float4 per half-res pixel (0 for non-ray pixels)

// Must match Engine/Rendergraph/ssr.h SsrClassifyPush (64 bytes).
// uint64 BDAs lead (see ssr.h — scalars never precede a BDA).
struct SsrClassifyPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<SsrRayList> rayList;   // packed uint per ray (halfW*halfH capacity)
    vk::BufferPointer<SsrRayCounter> rayCounter; // [0] = the count (reset by the args pass)
    vk::BufferPointer<SsrRadiance> radiance; // float4 per half-res pixel (0 for non-rays)
    uint gbufferTextureIndex; // thin G-buffer (RGBA8: oct normal + roughness + metal)
    uint depthTextureIndex;  // full-res depth (validity + receiver selection)
    float glossThreshold; // rays wanted for roughness < this (mirrors < 0.0001 always shoot)
    uint halfW;           // half-res reflection extent (the ray coords domain)
    uint halfH;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::push_constant]]
ConstantBuffer<SsrClassifyPush> sp;

// Mirror reflections (roughness < 0.0001) always shoot a full ray and
// never reduce (same rule as FidelityFX SSSR IsMirrorReflection).
bool IsMirrorReflection(float roughness)
{
    return roughness < 0.0001f;
}

groupshared uint g_TileCount;

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    bool is_first_lane_of_wave = WaveIsFirstLane();

    uint2 px = tid.xy;
    bool inRange = (px.x < sp.halfW) && (px.y < sp.halfH);

    // Receiver = nearest valid opaque tap of the full-res 2x2 block
    // (reverse-Z max — same discipline as the old trace's receiver
    // selection; never fabricate a center across a silhouette).
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    int2 fullBase = int2(px) * 2;
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
    bool valid = inRange && !DofIsBackgroundOrGarbage(dBest);

    // Roughness from the thin G-buffer at the receiver pixel.
    float roughness = 1.0f;
    if (valid)
    {
        Texture2D gbTex = textures[NonUniformResourceIndex(sp.gbufferTextureIndex)];
        roughness = gbTex.Load(int3(fullPxC, 0)).b;
    }

    bool isMirror = valid && IsMirrorReflection(roughness);
    bool isGlossy = valid && (roughness < sp.glossThreshold);
    // Offscreen/background/matte pixels never shoot; mirrors always do.
    bool needs_ray = isGlossy;

    // The samples-per-quad reduction is compiled in as the copy scheme
    // (copy flags pack per ray) but wired at 4 samples/quad for now
    // (every glossy + mirror pixel shoots its own ray — same density as
    // the old fullscreen trace; dropping to 1/2 is a one-line change
    // once the blur is validated against the copies).
    bool is_base_ray = true;
    needs_ray = needs_ray && is_base_ray;

    // Non-ray pixels write silence directly (mirrors the SDK's
    // zero-init + matte handling; the miss-silence keeps the analytic
    // sky IBL the sole sky contributor — no envmap double-count).
    // Every pixel is owned by exactly one writer per frame.
    uint flatIdx = px.y * sp.halfW + px.x;
    if (inRange && !needs_ray)
        sp.radiance.Get().v[flatIdx] = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // Wave-compact the rays: dense per-wave prefix + one atomic append
    // per wave (the first lane), then each shooting thread stores its
    // packed coords at its compacted index.
    uint local_ray_index_in_wave = WavePrefixCountBits(needs_ray);
    uint wave_ray_count = WaveActiveCountBits(needs_ray);
    uint base_ray_index = 0;
    if (is_first_lane_of_wave)
    {
        uint ignored;
        InterlockedAdd(sp.rayCounter.Get().count[0], wave_ray_count, ignored);
        base_ray_index = ignored;
    }
    base_ray_index = WaveReadLaneFirst(base_ray_index);
    if (needs_ray)
    {
        uint ray_index = base_ray_index + local_ray_index_in_wave;
        uint packed = 0;
        SsrPackRayCoords(px, false, false, false, packed);
        sp.rayList.Get().rays[ray_index] = packed;
    }
}
