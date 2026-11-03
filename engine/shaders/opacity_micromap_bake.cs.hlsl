// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// One GPU invocation per opaque MASK triangle. Sample its baseColor alpha
// at the centroids of its 16 subdivision-2 microtriangles, then encode the
// 2-state OMM bits in Vulkan's bird-curve order. This is a one-time
// per-BLAS bake (not a per-frame ray-tracing shader).
#include "common.hlsli"
#include "gpu_platform.hlsli"

struct OmmWords { uint words[]; };
struct OmmBakePush
{
    GPU_PTR_READ(OmmWords) indices;
    GPU_PTR_READ(OmmWords) shading;
    GPU_PTR_WRITE(OmmWords) output;
    uint firstIndex;
    int baseVertex;
    uint triangleCount;
    uint baseColorTexture; // bindless index, UINT32_MAX means factor only
    float baseAlphaFactor;
    float alphaCutoff;
};
GPU_PUSH(OmmBakePush, sp);

float2 unpackUv(uint vertexIndex)
{
    // 16-byte PackedVertexShading, UV is the SECOND uint, RG16 half.
    uint word = sp.shading.Get().words[vertexIndex * 4u + 1u];
    return float2(f16tof32(word & 65535u), f16tof32(word >> 16u));
}

uint spreadBits(uint x)
{
    x = (x | (x << 8u)) & 0x00ff00ffu;
    x = (x | (x << 4u)) & 0x0f0f0f0fu;
    x = (x | (x << 2u)) & 0x33333333u;
    x = (x | (x << 1u)) & 0x55555555u;
    return x;
}

// VK_EXT_opacity_micromap's reference barycentric -> bird-curve index.
uint birdIndex(float u, float v)
{
    const uint N = 4u; // subdivision level 2
    float fu = saturate(u) * float(N);
    float fv = saturate(v) * float(N);
    uint iu = min((uint)fu, N - 1u);
    uint iv = min((uint)fv, N - 1u);
    float uf = frac(fu);
    float vf = frac(fv);
    uint iuv = iu + iv;
    if (iuv >= N)
        iu -= iuv - N + 1u;
    uint iw = ~(iu + iv);
    if (uf + vf >= 1.0f && iuv < N - 1u)
        --iw;
    uint b0 = ~(iu ^ iw) & (N - 1u);
    uint t = (iu ^ iv) & b0;
    uint f = t;
    f ^= f >> 1u;
    f ^= f >> 2u;
    f ^= f >> 4u;
    f ^= f >> 8u;
    uint b1 = ((f ^ iu) & ~b0) | t;
    return spreadBits(b0) | (spreadBits(b1) << 1u);
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    const uint triangle = tid.x;
    if (triangle >= sp.triangleCount)
        return;

    const uint i = sp.firstIndex + triangle * 3u;
    const uint a = uint(sp.baseVertex) + sp.indices.Get().words[i + 0u];
    const uint b = uint(sp.baseVertex) + sp.indices.Get().words[i + 1u];
    const uint c = uint(sp.baseVertex) + sp.indices.Get().words[i + 2u];
    const float2 t0 = unpackUv(a);
    const float2 t1 = unpackUv(b);
    const float2 t2 = unpackUv(c);
    uint mask = 0u;

    // Subdivision-2 regular triangular grid has 10 upright and 6
    // inverted microtriangles. No approximated row-major bit index:
    // convert each centroid to the spec-defined bird curve.
    [loop]
    for (uint y = 0u; y < 4u; ++y)
    {
        [loop]
        for (uint x = 0u; x + y < 4u; ++x)
        {
            [unroll]
            for (uint inverted = 0u; inverted < 2u; ++inverted)
            {
                if (inverted != 0u && x + y >= 3u)
                    continue;
                float offset = (inverted == 0u) ? (1.0f / 3.0f) : (2.0f / 3.0f);
                float u = (float(x) + offset) * 0.25f;
                float v = (float(y) + offset) * 0.25f;
                float2 uv = t0 * (1.0f - u - v) + t1 * u + t2 * v;
                float alpha = sp.baseAlphaFactor;
                if (sp.baseColorTexture != 0xffffffffu)
                    alpha *= textures[NonUniformResourceIndex(sp.baseColorTexture)]
                        .SampleLevel(samplers[0], uv, 0).a;
                if (alpha >= sp.alphaCutoff)
                    mask |= 1u << birdIndex(u, v);
            }
        }
    }
    // One 32-bit word per triangle (16 little-endian 2-state bits,
    // upper 16 zero). Each VkMicromapTriangleEXT points at +4*triangle.
    sp.output.Get().words[triangle] = mask;
}
