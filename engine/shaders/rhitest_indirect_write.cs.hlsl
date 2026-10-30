// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// rhitest_indirect_write.cs.hlsl - the RHI upload-barrier regression's
// compute half (tests/test_rhi_upload_barriers_gpu.cpp; the PR2 spec).
// Writes ONLY the DYNAMIC halves of a VkDrawIndexedIndirectCommand
// (instanceCount = word 1, firstInstance = word 4) through BDA: the
// STATIC halves (indexCount/firstIndex/vertexOffset) stay owned by a
// same-frame UploadDataToBuffer - the split-writer sequence production
// placement retired with PR1 (see todo.txt), reproduced here
// INTENTIONALLY at the RHI level so the class cannot quietly return.
//
// Deliberately include-free: the TU defines its own tiny push + buffer
// view (no common.hlsli, no pass guards - nothing but the BDA intrinsic).

// Flat 20-byte VkDrawIndexedIndirectCommand view (word indices):
//   [0] indexCount  [1] instanceCount  [2] firstIndex
//   [3] vertexOffset [4] firstInstance
#include "gpu_platform.hlsli"

struct RhiTestIndirectBuffer { uint v[5]; };

// Mirrors the test's RhiTestIndirectWritePush (16 bytes).
struct RhiTestIndirectWritePush
{
    GPU_PTR_WRITE(RhiTestIndirectBuffer) indirect; // 0
    uint instanceCount;                                 // 8
    uint firstInstance;                                 // 12
};

GPU_PUSH(RhiTestIndirectWritePush, pc);

[numthreads(1, 1, 1)]
void mainCS()
{
    pc.indirect.Get().v[1] = pc.instanceCount;
    pc.indirect.Get().v[4] = pc.firstInstance;
}
