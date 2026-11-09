// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU Scene dense meshlet stream -> #301 four-PSO prefix count.
// One 256-thread workgroup per candidate block; no global append atomics.
// The class scan is the unchanged gpu_scene_pso_prefix_scan.cs.hlsl.
#include "gpu_platform.hlsli"
#define MAX_CANDIDATES 1048576u
#define MAX_BLOCKS 4096u
#define PSO_CLASSES 4u
struct Candidate { uint4 draw; uint4 meta; };
struct Candidates { Candidate rows[MAX_CANDIDATES]; };
struct CandidateOffsets { uint values[MAX_CANDIDATES+1u]; };
struct BlockCounts { uint values[MAX_BLOCKS*PSO_CLASSES]; };
struct Push {
    GPU_PTR_READ(Candidates) candidates;
    GPU_PTR_READ(CandidateOffsets) meshletOffsets;
    GPU_PTR_WRITE(BlockCounts) blockCounts;
    uint workCapacity;        // length used by the work-prefix scan
    uint candidateCapacity;   // allocated dense candidate capacity
    uint blockCount;          // ceil(candidateCapacity/256)
    uint reserved;
};
GPU_PUSH(Push, pc);
groupshared uint waveCounts[PSO_CLASSES*256u];
[numthreads(256,1,1)]
void mainCS(uint3 gid: SV_GroupID, uint3 lane: SV_GroupThreadID) {
    if (gid.x>=pc.blockCount || pc.blockCount>MAX_BLOCKS) return;
    const uint total=pc.workCapacity<=MAX_CANDIDATES
        ? pc.meshletOffsets.Get().values[pc.workCapacity] : 0u;
    const uint i=gid.x*256u+lane.x;
    const bool active=total<=pc.candidateCapacity && i<total &&
                      i<MAX_CANDIDATES;
    const uint cls=active?pc.candidates.Get().rows[i].meta.x:0xffffffffu;
    const uint wave=lane.x/WaveGetLaneCount();
    [unroll] for(uint c=0u;c<PSO_CLASSES;++c) {
        uint n=WaveActiveCountBits(cls==c);
        if(WaveIsFirstLane())waveCounts[c*256u+wave]=n;
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane.x<PSO_CLASSES) {
        const uint waves=(256u+WaveGetLaneCount()-1u)/WaveGetLaneCount();
        uint sum=0u;
        for(uint w=0u;w<waves;++w)sum+=waveCounts[lane.x*256u+w];
        pc.blockCounts.Get().values[gid.x*PSO_CLASSES+lane.x]=sum;
    }
}
