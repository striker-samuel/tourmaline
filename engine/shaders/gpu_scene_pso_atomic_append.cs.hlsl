// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Candidate E — wave-aggregated atomic append (prototype). Replaces the
// three-pass atomics-free prefix (gpu_scene_pso_prefix_count/scan/scatter)
// with ONE pass: each wave counts its valid work per PSO class, reserves the
// output range with a single InterlockedAdd, and writes the indirect commands
// at the reserved slots.
//
// Trade-offs vs the prefix path (the doc's comparison points):
//   - memory traffic: one global atomic per wave per class (vs. the block
//     totals + block bases round-trip); the 4x block-totals/bases buffers go.
//   - order: non-deterministic across waves (stable within a wave); the prefix
//     path is fully deterministic. firstInstance still = original work index.
//   - contention: only the class `next` counters are contended, once per wave.
//
// NOT wired into the renderer: the host must (1) reset each class `next` to 0
// with an explicit dependency before this pass (the running 0-based count IS
// the per-class draw count read by the device), (2) read the final `next`
// per class as the countBuffer for vkCmdDrawIndexedIndirectCount, and (3)
// define a no-dropped-draws fallback for any overflow (clamping counts is NOT
// valid — see the PSO prefix doc).
#include "gpu_platform.hlsli"

#define PSO_MAX_WORK 1048576u
#define PSO_MAX_GEO 65535u
#define PSO_MAX_CANDIDATES 1048576u
#define PSO_CLASSES 4u
#define PSO_GROUP 256u
#define PSO_INVALID 0xffffffffu

struct Work { uint4 identity; uint4 detail; };
struct WorkRows { Work rows[PSO_MAX_WORK]; };
struct WorkOffsets { uint values[PSO_MAX_CANDIDATES + 1u]; };
struct IndexedGeometry { uint indexCount; uint firstIndex; int vertexOffset; uint flags; };
struct GeometryRows { IndexedGeometry rows[PSO_MAX_GEO]; };
struct Aliases { uint slots[PSO_MAX_GEO]; };
struct RasterIndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct IndirectRows { RasterIndirectCmd rows[PSO_MAX_WORK]; };
// Per-class append state: `next` is the running 0-based count (reset to 0 by
// the host; its final value is the per-class draw count), `overflow` is a
// sticky OR of error bits.
struct ClassState { uint next; uint overflow; uint pad0; uint pad1; };
struct ClassStates { ClassState classes[PSO_CLASSES]; };

struct Push {
    GPU_PTR_READ(WorkRows) work;
    GPU_PTR_READ(WorkOffsets) workOffsets;
    GPU_PTR_READ(GeometryRows) geometry;
    GPU_PTR_READ(Aliases) aliases;
    GPU_PTR_WRITE(IndirectRows) indirect;
    GPU_PTR(ClassStates) classStates; // RW: atomic append + overflow flags
    uint candidateCount;
    uint workCapacity;
    uint geometryCount;
    uint aliasCount;
    uint commandCapacity;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint4 binOffsets;
    uint4 binCapacities;
};
GPU_PUSH(Push, pc);

[numthreads(PSO_GROUP, 1, 1)]
void mainCS(uint3 gid : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    const uint i = gid.x * PSO_GROUP + lane.x;
    const uint total = pc.candidateCount <= PSO_MAX_CANDIDATES
                           ? pc.workOffsets.Get().values[pc.candidateCount]
                           : 0u;
    const bool inRange = total <= pc.workCapacity && i < total &&
                         i < pc.workCapacity && i < PSO_MAX_WORK;
    Work work = (Work)0;
    if (inRange)
        work = pc.work.Get().rows[i];

    const uint transientId = work.identity.z;
    const bool hasAlias = inRange && transientId < pc.aliasCount &&
                          transientId < PSO_MAX_GEO;
    uint geometryId = PSO_INVALID;
    if (hasAlias)
        geometryId = pc.aliases.Get().slots[transientId];
    const bool hasGeometry = hasAlias && geometryId < pc.geometryCount &&
                             geometryId < PSO_MAX_GEO;
    IndexedGeometry g = (IndexedGeometry)0;
    if (hasGeometry)
        g = pc.geometry.Get().rows[geometryId];
    const uint cls = work.detail.z;
    const bool valid = hasGeometry && g.indexCount != 0u &&
                       work.identity.x != PSO_INVALID && work.identity.y != 0u &&
                       cls < PSO_CLASSES;

    // Wave-aggregated append: count the valid lanes per class, then have the
    // first lane of each wave reserve the range with ONE InterlockedAdd.
    const uint waveSize = WaveGetLaneCount();
    [unroll]
    for (uint c = 0u; c < PSO_CLASSES; ++c) {
        const bool selected = valid && cls == c;
        const uint countInWave = WaveActiveCountBits(selected);
        const uint rankInWave = WavePrefixCountBits(selected);
        uint base = 0u;
        if (WaveIsFirstLane() && countInWave != 0u)
            InterlockedAdd(pc.classStates.Get().classes[c].next, countInWave, base);
        base = WaveReadLaneFirst(base);
        if (!selected)
            continue;

        const uint slot = pc.binOffsets[c] + base + rankInWave; // base is 0-based
        const uint capacity = pc.binCapacities[c];
        // Guards are diagnostic only — the host MUST handle overflow without
        // dropping any scene draw before this path is enabled.
        if (pc.binOffsets[c] > pc.commandCapacity ||
            capacity > pc.commandCapacity - pc.binOffsets[c] ||
            base + rankInWave >= capacity || slot >= PSO_MAX_WORK) {
            InterlockedOr(pc.classStates.Get().classes[c].overflow, 1u);
            continue;
        }
        RasterIndirectCmd cmd;
        cmd.indexCount = g.indexCount;
        cmd.instanceCount = 1u;
        cmd.firstIndex = g.firstIndex;
        cmd.vertexOffset = g.vertexOffset;
        cmd.firstInstance = i; // preserve the original persistent-instance validation
        pc.indirect.Get().rows[slot] = cmd;
    }
}
