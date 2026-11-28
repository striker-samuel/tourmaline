// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Candidate E — wave-aggregated atomic append for the dense meshlet
// candidate stream. Replaces the three-pass atomics-free PSO prefix
// (meshlet_pso_count -> pso_prefix_scan -> meshlet_pso_scatter) with ONE
// pass: each wave reserves its per-class output range with a single
// InterlockedAdd on the classCounts word, so that word ends up equal to the
// per-class draw count the device reads for vkCmdDrawIndexedIndirectCount.
// firstInstance stays the ORIGINAL GPU Scene work slot (draw.w), so the
// unmodified raster VS (work[SV_InstanceID]) and temporal identity contract
// hold. Trade-offs vs the prefix path (the doc's comparison points):
//   - memory traffic: one global atomic per wave per class; the block
//     totals + block bases buffers disappear.
//   - order: non-deterministic across waves (stable within a wave); the
//     prefix path is fully deterministic. firstInstance is unaffected.
//   - contention: only the class `counts` words are contended, once per wave.
#include "gpu_platform.hlsli"
#define MAX_CANDIDATES 1048576u
#define CLASSES 4u
#define MAX_COMMANDS (CLASSES * MAX_CANDIDATES)
struct Candidate { uint4 draw; uint4 meta; };
struct Candidates { Candidate rows[MAX_CANDIDATES]; };
struct Offsets { uint values[MAX_CANDIDATES + 1u]; };
struct ClassCounts { uint counts[4]; uint overflow[4]; };
struct Cmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
// Four per-class command ranges of candidateCapacity (see the scatter kernel:
// a legal bin-3 destination can reach 4M-1 even though the source stream is
// capped at 1M).
struct Commands { Cmd rows[MAX_COMMANDS]; };
struct Push {
    GPU_PTR_READ(Candidates) candidates;
    GPU_PTR_READ(Offsets) meshletOffsets;
    GPU_PTR(ClassCounts) classCounts; // RW: atomic append + overflow flags
    GPU_PTR_WRITE(Commands) commands;
    uint workCapacity;        // index of the total in meshletOffsets
    uint candidateCapacity;   // allocated dense candidate capacity
    uint commandCapacity;     // sum of bin capacities
    uint reserved;
    uint4 binOffsets;
    uint4 binCapacities;
};
GPU_PUSH(Push, pc);
[numthreads(256, 1, 1)]
void mainCS(uint3 gid : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    const uint total = pc.workCapacity <= MAX_CANDIDATES
                           ? pc.meshletOffsets.Get().values[pc.workCapacity]
                           : 0u;
    const uint i = gid.x * 256u + lane.x;
    const bool inRange = total <= pc.candidateCapacity && i < total &&
                         i < MAX_CANDIDATES;
    // Struct-typed conditional results are rejected by DXC (numeric-only
    // ternaries); declare-then-assign, the scatter kernel's pattern.
    Candidate c = (Candidate)0;
    if (inRange)
        c = pc.candidates.Get().rows[i];
    // Match the scatter predicate exactly: a classified candidate with a
    // zero index count produces no command and must not increment the count.
    const uint cls = inRange ? c.meta.x : 0xffffffffu;
    const bool valid = cls < CLASSES && c.draw.y != 0u;

    [unroll]
    for (uint k = 0u; k < CLASSES; ++k) {
        const bool selected = valid && cls == k;
        const uint n = WaveActiveCountBits(selected);
        const uint rank = WavePrefixCountBits(selected);
        uint base = 0u;
        if (WaveIsFirstLane() && n != 0u)
            InterlockedAdd(pc.classCounts.Get().counts[k], n, base);
        base = WaveReadLaneFirst(base);
        if (!selected)
            continue;

        const uint offset = pc.binOffsets[k];
        const uint capacity = pc.binCapacities[k];
        // Guards are diagnostic only: the CPU conservative bound makes a real
        // overflow impossible, and the host MUST never submit truncated draws
        // (see the PSO prefix doc's no-dropped-draws requirement).
        if (offset > pc.commandCapacity ||
            capacity > pc.commandCapacity - offset ||
            base >= capacity || rank >= capacity - base ||
            offset + base + rank >= MAX_COMMANDS) {
            InterlockedOr(pc.classCounts.Get().overflow[k], 2u);
            continue;
        }
        Cmd cmd;
        cmd.indexCount = c.draw.y;
        cmd.instanceCount = 1u;
        cmd.firstIndex = c.draw.x;
        cmd.vertexOffset = asint(c.draw.z);
        cmd.firstInstance = c.draw.w;
        pc.commands.Get().rows[offset + base + rank] = cmd;
    }
}
