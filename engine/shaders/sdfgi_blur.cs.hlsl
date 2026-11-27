// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#include "common.hlsli"
#include "dof.hlsli"

// Screen-adaptive SDF GI (#410) spatial resolve: a depth-aware 5x5 box blur
// over the tile-grid irradiance history. The per-tile cosine-weighted
// estimate is Monte-Carlo noisy (~15% at 6 rays/tile even after the EMA),
// and diffuse irradiance is inherently low-frequency, so a spatial smooth
// removes the tile-to-tile variance before Main samples it.
//
// The blur is depth-aware (a bilateral weight): taps whose depth differs
// from the centre by more than a threshold are dropped, so foreground light
// does not bleed onto the background across a silhouette, through thin
// geometry, or across a foreground/background step. Reads history[slot]
// (the raw field the trace just wrote) and writes a transient smoothed copy.

[[vk::binding(2, 0)]]
RWTexture2D<float4> sdfGiBlurStorage[];

struct SdfGiBlurPush {
    uint srcImageIndex; // history[writeSlot] (sampled read)
    uint outImageIndex; // smoothed output (storage write)
    uint outWidth;
    uint outHeight;
    uint depthTextureIndex; // full-res opaque depth (bilateral guide)
    uint samplerIndex;
    uint pad0, pad1;
};
GPU_PUSH(SdfGiBlurPush, sp);

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= sp.outWidth || tid.y >= sp.outHeight)
        return;

    const float2 uv = (float2(tid.xy) + 0.5f) /
                      float2(float(sp.outWidth), float(sp.outHeight));
    Texture2D src = textures[NonUniformResourceIndex(sp.srcImageIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];

    const float3 raw = src.SampleLevel(smp, uv, 0).rgb;
    const float3 centre = all(isfinite(raw)) ? raw : float3(0.0f, 0.0f, 0.0f);

    // Depth guide: sample the full-res depth at the tile centre and, for each
    // tap, drop the tap if it crosses a depth discontinuity (relative to the
    // reversed-Z depth value, so near and far surfaces get scale-appropriate
    // thresholds).
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    const float ndcC = depthTex.SampleLevel(smp, uv, 0).r;
    const float discThreshold = max(0.05f, ndcC * 0.2f);
    const float2 texel = 1.0f / float2(float(sp.outWidth), float(sp.outHeight));

    float3 sum = centre;
    float wsum = 1.0f;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            if (dx == 0 && dy == 0)
                continue;
            const float2 t = uv + float2(dx, dy) * texel;
            const float2 tc = clamp(t, texel * 0.5f, 1.0f - texel * 0.5f);
            // Depth-aware rejection: skip taps on a different surface.
            const float ndcT = depthTex.SampleLevel(smp, tc, 0).r;
            if (DofIsBackgroundOrGarbage(ndcT) || abs(ndcT - ndcC) >= discThreshold)
                continue;
            const float3 v = src.SampleLevel(smp, tc, 0).rgb;
            if (!all(isfinite(v)))
                continue;
            sum += v;
            wsum += 1.0f;
        }
    }
    sdfGiBlurStorage[sp.outImageIndex][tid.xy] = float4(sum / max(wsum, 1e-6f), 1.0f);
}
