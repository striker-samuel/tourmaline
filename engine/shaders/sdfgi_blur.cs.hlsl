// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#include "common.hlsli"

// Screen-adaptive SDF GI (#410) spatial resolve: a 5x5 box blur over the
// tile-grid irradiance history. The per-tile cosine-weighted estimate is
// Monte-Carlo noisy (~15% at 6 rays/tile even after the EMA), and diffuse
// irradiance is inherently low-frequency, so a spatial smooth removes the
// tile-to-tile variance before Main samples it. Reads history[slot] (the
// raw field the trace just wrote) and writes a transient smoothed copy.

[[vk::binding(2, 0)]]
RWTexture2D<float4> sdfGiBlurStorage[];

struct SdfGiBlurPush {
    uint srcImageIndex; // history[writeSlot] (sampled read)
    uint outImageIndex; // smoothed output (storage write)
    uint outWidth;
    uint outHeight;
    uint samplerIndex;
    uint pad0, pad1, pad2;
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

    // 5x5 box (clamp to the tile grid so edge tiles don't darken).
    const float2 texel = 1.0f / float2(float(sp.outWidth), float(sp.outHeight));
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            const float2 t = clamp(uv + float2(dx, dy) * texel,
                                   texel * 0.5f, 1.0f - texel * 0.5f);
            sum += src.SampleLevel(smp, t, 0).rgb;
        }
    }
    sdfGiBlurStorage[sp.outImageIndex][tid.xy] = float4(sum / 25.0f, 1.0f);
}
