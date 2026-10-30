// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "ssr_compute.hlsli"
#include "gpu_platform.hlsli"

// SSR indirect-args builder (SSSR prepare_indirect_args port — see
// FidelityFX-SDK-v1.1.4
// sdk/include/FidelityFX/gpu/sssr/ffx_sssr_prepare_indirect_args.h):
// single thread; reads the classify's ray counter, writes the
// intersect's VkDispatchIndirectCommand (x = ceil(rays/64) groups of 64,
// y = z = 1), then resets the counter for next frame. The denoiser-tile
// args are skipped (the bilateral blur is fullscreen — see
// ssr_classify.cs.hlsl).

// Must match Engine/Rendergraph/ssr.h SsrIndirectPush (16 bytes).
struct SsrIndirectPush
{
    GPU_PTR(SsrRayCounter) rayCounter; // [0] = the classify's count (reset here)
    GPU_PTR_WRITE(SsrIndirectArgs) indirectArgs; // VkDispatchIndirectCommand (3 uints)
};

GPU_PUSH(SsrIndirectPush, sp);

[numthreads(1, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x != 0 || tid.y != 0 || tid.z != 0)
        return;
    uint ray_count = sp.rayCounter.Get().count[0];
    sp.indirectArgs.Get().xyz[0] = (ray_count + 63u) / 64u;
    sp.indirectArgs.Get().xyz[1] = 1u;
    sp.indirectArgs.Get().xyz[2] = 1u;
    // Freeze the count for this frame's intersect (it early-outs past
    // the frozen count), then reset [0] for next frame's classify.
    sp.rayCounter.Get().count[1] = ray_count;
    sp.rayCounter.Get().count[0] = 0u;
}
