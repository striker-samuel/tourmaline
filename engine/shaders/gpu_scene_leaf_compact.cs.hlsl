// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// P1 leaf work queue (#372): compact ACTIVE leaves (view mask != 0) from the
// per-leaf mask produced by gpu_scene_leaf_classify.cs.hlsl into a bounded
// work list. Each entry carries the leaf's stable instance range ONCE, so the
// fused instance pass reads the leaf table here and never re-expands millions
// of instances into per-view candidate arrays.
//
// Compaction is BLOCK-AGGREGATED, not per-draw global atomics: each workgroup
// computes a block-level exclusive prefix of its active leaves, reserves a
// contiguous output range with one InterlockedAdd, and scatters its surviving
// leaves. A global prefix over millions of instances is thereby a prefix over
// far fewer workgroups.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GPU_SCENE_MAX_ROW_SLOTS 4194304u
#define LEAF_COMPACT_BLOCK 256u

struct GpuSceneLeaves {
    GpuSceneLeafRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneLeafMasks {
    uint masks[GPU_SCENE_MAX_ROW_SLOTS];
};
// One entry per ACTIVE leaf: leaf identity + its instance range + view mask.
struct GpuLeafWorkEntry {
    uint4 value; // x leafIndex, y viewMask, z firstMember, w memberCount
};
struct GpuLeafWorkQueue {
    GpuLeafWorkEntry entries[GPU_SCENE_MAX_ROW_SLOTS];
};
// Single device counter: the GPU-written active-leaf count (read by the next
// stage's dispatch-indirect; never read back to the CPU on a normal frame).
struct GpuLeafCounters {
    uint activeLeafCount;
    uint3 reserved;
};

struct GpuLeafCompactPush {
    GPU_PTR_READ(GpuSceneLeaves) leaves;
    GPU_PTR_READ(GpuSceneLeafMasks) masks;
    GPU_PTR_WRITE(GpuLeafWorkQueue) queue;
    GPU_PTR_WRITE(GpuLeafCounters) counters;
    uint4 counts; // x leafCount, y/w reserved, z zero counters (1 = reset)
};
GPU_PUSH(GpuLeafCompactPush, pcS);

groupshared uint s_waveActive[(LEAF_COMPACT_BLOCK + 31u) / 32u];
groupshared uint s_blockBase;

[numthreads(LEAF_COMPACT_BLOCK, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID) {
    // Reset the single counter on the first thread of the first group only,
    // so the InterlockedAdd below starts from zero every frame.
    if (gid.x == 0u && tid.x == 0u && (pcS.counts.z & 1u) != 0u) {
        pcS.counters.Get().activeLeafCount = 0u;
    }

    const uint leafIndex = gid.x * LEAF_COMPACT_BLOCK + tid.x;
    uint mask = 0u;
    if (leafIndex < pcS.counts.x && leafIndex < GPU_SCENE_MAX_ROW_SLOTS) {
        mask = pcS.masks.Get().masks[leafIndex];
    }
    const bool active = mask != 0u;

    // Wave-level exclusive prefix + count.
    const uint wavePrefix = WavePrefixCountBits(active);
    const uint waveActive = WaveActiveCountBits(active);
    const uint waveIndex = tid.x / 32u;
    if (WaveIsFirstLane()) {
        s_waveActive[waveIndex] = waveActive;
    }
    GroupMemoryBarrierWithGroupSync();

    // One thread reserves the block's contiguous output range.
    if (tid.x == 0u) {
        uint blockActive = 0u;
        for (uint w = 0u; w < (LEAF_COMPACT_BLOCK + 31u) / 32u; ++w)
            blockActive += s_waveActive[w];
        uint blockBase = 0u;
        if (blockActive != 0u)
            InterlockedAdd(pcS.counters.Get().activeLeafCount, blockActive, blockBase);
        s_blockBase = blockBase;
    }
    GroupMemoryBarrierWithGroupSync();

    if (!active)
        return;

    // Cross-wave prefix: sum the active counts of all preceding waves.
    uint crossWavePrefix = 0u;
    for (uint w = 0u; w < waveIndex; ++w)
        crossWavePrefix += s_waveActive[w];

    const uint outIndex = s_blockBase + crossWavePrefix + wavePrefix;
    if (outIndex >= GPU_SCENE_MAX_ROW_SLOTS)
        return;
    const GpuSceneLeafRow leaf = pcS.leaves.Get().rows[leafIndex];
    if (leaf.identity.y != 1u)
        return;
    GpuLeafWorkEntry entry;
    entry.value = uint4(leafIndex, mask, leaf.value.range.x, leaf.value.range.y);
    pcS.queue.Get().entries[outIndex] = entry;
}
