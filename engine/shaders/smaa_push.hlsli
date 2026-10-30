// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "gpu_platform.hlsli"

//--------------------------------------------------------------------------------------------------
// SMAA passes share one push layout (every pass binds the same block, each
// filling only the fields it reads; unused indices stay 0xffffffff).
//
// Must match SmaaPush in engine/include/Engine/Rendergraph/smaa.h byte for
// byte (88 bytes). All scalars on purpose: the engine's DXC
// -fvk-use-dx-layout packing gives them a plain 4-byte stride with no
// implicit padding, so the CPU mirror stays exact (size checks are C++-side;
// this toolchain's DXC rejects global-scope static_assert).
//--------------------------------------------------------------------------------------------------

struct SmaaPush
{
    uint colorTexIndex;      // edge/nb: LDR color input (smaa.ldr)
    uint edgesTexIndex;     // weights: edge-detection output
    uint areaTexIndex;      // weights: SMAA area LUT (160x560 RG8)
    uint searchTexIndex;    // weights: SMAA search LUT (64x16 R8)
    uint blendTexIndex;     // nb: blend-weights output
    uint currentTexIndex;   // resolve: this frame's nb output (history[slot])
    uint previousTexIndex;  // resolve: previous frame's nb output (history[slot^1])
    uint velocityTexIndex;  // nb/resolve: motion vectors (RG16F, prevUV-currUV)
    uint linearSamplerIndex;  // edges/area/search/blend/color (clamp)
    uint pointSamplerIndex;   // edge color input + resolve fetches (clamp)
    uint historyValid;      // resolve: 0 = first T2x frame, passthrough current
    float subsampleIdxX;    // weights: T2x subsample indices (see smaa.hlsli
    float subsampleIdxY;    //   @SUBSAMPLE_INDICES: phase A = (1,1,1,0),
    float subsampleIdxZ;    //   phase B = (2,2,2,0), SMAA 1x = (0,0,0,0))
    float subsampleIdxW;
    float rtMetricsX;       // float4(1/w, 1/h, w, h) at swap resolution
    float rtMetricsY;
    float rtMetricsZ;
    float rtMetricsW;
    uint outputLinear;      // nb/resolve: 1 = decode to linear for the sRGB
                            //   swapchain store, 0 = keep encoded bytes
                            //   (UNORM history target)
    float jitterDeltaX;     // nb only: (prevJitter - currJitter) in UV; the
    float jitterDeltaY;     //   packed velocity magnitude strips it so the
                            //   resolve attenuation sees true motion only
};

GPU_PUSH(SmaaPush, sp);
