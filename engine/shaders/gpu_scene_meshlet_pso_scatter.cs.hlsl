// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// PSO-compacted meshlet indexed indirect draws: #301 block bases + wave ranks.
// FirstInstance remains ORIGINAL GPU Scene submesh work slot, never the
// compacted command ordinal. The CPU must reject flagged overflow before
// admitting compact ownership. This kernel is staged, not yet dispatched.
#include "gpu_platform.hlsli"
#define MAX_CANDIDATES 1048576u
#define MAX_BLOCKS 4096u
#define CLASSES 4u
struct Candidate { uint4 draw; uint4 meta; };
struct Candidates { Candidate rows[MAX_CANDIDATES]; };
struct Offsets { uint values[MAX_CANDIDATES+1u]; };
struct BlockTotals { uint values[MAX_BLOCKS*CLASSES]; };
struct ClassCounts { uint counts[4]; uint overflow[4]; };
struct Cmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
struct Commands { Cmd rows[MAX_CANDIDATES]; };
struct Push {
    GPU_PTR_READ(Candidates) candidates;
    GPU_PTR_READ(Offsets) meshletOffsets;
    GPU_PTR_READ(BlockTotals) blockBases;
    GPU_PTR(ClassCounts) classCounts;
    GPU_PTR_WRITE(Commands) commands;
    uint workCapacity;
    uint candidateCapacity;
    uint blockCount;
    uint commandCapacity;
    uint pad0;
    uint pad1;
    uint4 binOffsets;
    uint4 binCapacities;
};
GPU_PUSH(Push, pc);
groupshared uint waveCounts[CLASSES*256u];
[numthreads(256,1,1)]
void mainCS(uint3 gid: SV_GroupID,uint3 lane: SV_GroupThreadID) {
    if(gid.x>=pc.blockCount || pc.blockCount>MAX_BLOCKS) return;
    const uint total=pc.workCapacity<=MAX_CANDIDATES
        ? pc.meshletOffsets.Get().values[pc.workCapacity] : 0u;
    const uint i=gid.x*256u+lane.x;
    const bool inRange=total<=pc.candidateCapacity && i<total &&
                       i<MAX_CANDIDATES;
    // Struct-typed conditional results are rejected by DXC (numeric-only
    // ternaries); declare-then-assign, the #301 scatter kernel's pattern.
    Candidate c=(Candidate)0;
    if(inRange) c=pc.candidates.Get().rows[i];
    const uint cls=inRange?c.meta.x:0xffffffffu;
    const bool valid=cls<CLASSES && c.draw.y!=0u;
    const uint wave=lane.x/WaveGetLaneCount();
    uint localRank=0u;
    [unroll] for(uint k=0u;k<CLASSES;++k) {
        const bool selected=valid && cls==k;
        const uint n=WaveActiveCountBits(selected);
        const uint rank=WavePrefixCountBits(selected);
        if(WaveIsFirstLane())waveCounts[k*256u+wave]=n;
        if(selected)localRank=rank;
    }
    GroupMemoryBarrierWithGroupSync();
    if(!valid)return;
    for(uint w=0u;w<wave;++w)localRank+=waveCounts[cls*256u+w];
    const uint base=pc.blockBases.Get().values[gid.x*CLASSES+cls];
    const uint offset=pc.binOffsets[cls],capacity=pc.binCapacities[cls];
    if(offset>pc.commandCapacity || capacity>pc.commandCapacity-offset ||
       base>=capacity || localRank>=capacity-base ||
       offset+base+localRank>=MAX_CANDIDATES) {
        InterlockedOr(pc.classCounts.Get().overflow[cls],2u);
        return;
    }
    Cmd cmd;
    cmd.indexCount=c.draw.y;
    cmd.instanceCount=1u;
    cmd.firstIndex=c.draw.x;
    cmd.vertexOffset=asint(c.draw.z);
    cmd.firstInstance=c.draw.w;
    pc.commands.Get().rows[offset+base+localRank]=cmd;
}
