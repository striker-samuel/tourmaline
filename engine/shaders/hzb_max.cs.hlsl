// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Hierarchical-Z build, MAX-reduce variant for the SSR trace (SSSR-style
// Hi-Z input — see ssr.h + ssr.frag.hlsl).
//
// The CULLING pyramid (hzb.cs.hlsl, owned by the gfx filter) reduces MIN
// (farthest surface per block — the occlusion-cull conservatism) and is
// built over the seed's partial set or Option A's crudest-LOD occluders,
// before the main depth passes complete. Ray TRACING wants the opposite:
// the NEAREST surface per block (MAX under reversed-Z: depth 1 = near,
// 0 = far/sky) over the COMPLETE current-frame renderDepth — the trace's
// empty-space skip proves "every surface in the covered block is behind
// the ray" from the block's nearest (largest reversed-Z) value.
//
// Same buffer layout, mip chain and push as the culling build (HzbPush,
// 48 bytes — one buffer, manual mips, BDA storage): mip 0 mirrors the
// depth texture 1:1; mips 1+ are MAX of the clamped 2x2 footprint below.
// Emitted by the app AFTER all depth passes complete (the trace runs
// after Main — the depth is final by then), unlike the culling chain.
//
// Zero-fill initialization reads as far/sky at every block (MAX stays 0
// until built) — a garbage/unbuilt block's nearest reads behind
// everything, so the trace's skip never crosses unknown content; it can
// only skip a provably-empty block (a missed reflection at worst, never
// a see-through one).

#define HZB_PASS
#include "common.hlsli"
#include "hzb.hlsli"
#include "gpu_platform.hlsli"

GPU_PUSH(HzbPush, pcH);

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    uint dstW = pcH.dstWidth;
    uint dstH = pcH.dstHeight;
    if (tid.x >= dstW || tid.y >= dstH)
        return;
    uint dstIdx = pcH.dstOffset + tid.y * dstW + tid.x;

    float v;
    if (pcH.srcMip == HZB_SRC_DEPTH)
    {
        // Mip 0: exact copy (depth res == HZB res by construction).
        uint depthIdx = NonUniformResourceIndex(pcH.depthTextureIndex);
        v = textures[depthIdx].Load(int3((int)tid.x, (int)tid.y, 0)).r;
    }
    else
    {
        // MAX of clamped 2x2 footprint: the NEAREST surface in the block
        // (largest reversed-Z; one sky pixel contributes 0 = far and
        // never wins the MAX — geometry dominates its block correctly).
        uint2 base = uint2(tid.x * 2u, tid.y * 2u);
        uint sx1 = min(base.x + 1u, pcH.srcWidth - 1u);
        uint sy1 = min(base.y + 1u, pcH.srcHeight - 1u);
        float a = pcH.hzb.Get().texels[pcH.srcOffset + base.y * pcH.srcWidth + base.x];
        float b = pcH.hzb.Get().texels[pcH.srcOffset + base.y * pcH.srcWidth + sx1];
        float c = pcH.hzb.Get().texels[pcH.srcOffset + sy1 * pcH.srcWidth + base.x];
        float d = pcH.hzb.Get().texels[pcH.srcOffset + sy1 * pcH.srcWidth + sx1];
        v = max(max(a, b), max(c, d));
    }
    // Sanitize: NaN/Inf poison the chain upward (MAX keeps NaN). Depth is
    // in [0,1] by construction; collapse non-finite to 0 (far/sky — the
    // skip-safe direction: reads as "nothing here").
    if (!isnan(v) && !isinf(v))
        pcH.hzb.Get().texels[dstIdx] = v;
    else
        pcH.hzb.Get().texels[dstIdx] = 0.0f;
}
