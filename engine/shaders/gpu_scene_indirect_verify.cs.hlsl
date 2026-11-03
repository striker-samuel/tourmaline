// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU-driven indirect dispatch consumer: each thread copies exactly one
// ordered work record to the parity buffer, proving the args are usable.
// Real indexed-draw generation requires registered geometry index metadata.
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 32768u
#define GS_MAX_WORK 524288u
struct Offsets { uint values[GS_MAX_CANDIDATES + 1u]; };
struct WorkRow { uint4 identity; uint4 detail; };
struct WorkBuffer { WorkRow rows[GS_MAX_WORK]; };
struct Push {
    GPU_PTR_READ(Offsets) offsets;
    GPU_PTR_READ(WorkBuffer) work;
    GPU_PTR_WRITE(WorkBuffer) verified;
    uint candidateCount;
    uint workCapacity;
};
GPU_PUSH(Push, pcI);
[numthreads(64,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    if (pcI.candidateCount > GS_MAX_CANDIDATES) return;
    const uint count = pcI.offsets.Get().values[pcI.candidateCount];
    if (count > min(pcI.workCapacity, GS_MAX_WORK) || tid.x >= count) return;
    pcI.verified.Get().rows[tid.x] = pcI.work.Get().rows[tid.x];
}
