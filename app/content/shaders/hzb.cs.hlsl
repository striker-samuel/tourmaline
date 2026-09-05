// Hierarchical-Z build (occlusion culling input, Alan Wake 2 style).
//
// One dispatch per mip level (caller loops; rendergraph barriers come from
// one compute pass per level — there is no intra-pass barrier API):
//   - mip 0: exact 1:1 copy of the depth texture (srcMip = HZB_SRC_DEPTH).
//   - mips 1+: MIN of the clamped 2x2 footprint in the level below.
//
// Reversed-Z (1 = near, 0 = far): reduction is MIN — see hzb.hlsli for the
// conservatism derivation. Depth loads use pixel coords (framebuffer y-down
// == texture rows, same as forwardplus.cs.hlsl), so mip 0 needs no flip.
//
// Buffers arrive via buffer-device-address push (vk::BufferPointer, same as
// gfxfilter.cs.hlsl) so no descriptor/pipeline-layout changes are needed.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (HzbPush, 48 bytes).

#define HZB_PASS
#include "common.hlsli"
#include "hzb.hlsli"

[[vk::push_constant]]
ConstantBuffer<HzbPush> pcH;

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
        // MIN of clamped 2x2 footprint (conservative under reversed-Z).
        uint2 base = uint2(tid.x * 2u, tid.y * 2u);
        uint sx1 = min(base.x + 1u, pcH.srcWidth - 1u);
        uint sy1 = min(base.y + 1u, pcH.srcHeight - 1u);
        float a = pcH.hzb.Get().texels[pcH.srcOffset + base.y * pcH.srcWidth + base.x];
        float b = pcH.hzb.Get().texels[pcH.srcOffset + base.y * pcH.srcWidth + sx1];
        float c = pcH.hzb.Get().texels[pcH.srcOffset + sy1 * pcH.srcWidth + base.x];
        float d = pcH.hzb.Get().texels[pcH.srcOffset + sy1 * pcH.srcWidth + sx1];
        v = min(min(a, b), min(c, d));
    }
    pcH.hzb.Get().texels[dstIdx] = v;
}
