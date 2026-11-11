// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Shared exact-identity GPU Scene meshlet temporal history operations.
// A set entry is only committed after all three key words are written.
// Collision/probe exhaustion never drops Main geometry: overflow fails open.
#define GS_TEMPORAL_PROBES 32u
uint MeshletHistoryHash(uint slot,uint gen,uint clusterId) {
    uint h=slot*0x9e3779b9u;
    h=(h^(gen*0x85ebca6bu))*0xc2b2ae35u;
    h=(h^(clusterId*0x27d4eb2du))*0x165667b1u;
    return h^(h>>16u);
}
bool MeshletHistoryContains(GPU_PTR(GsTemporalHistory) history,uint capacity,
                            uint slot,uint gen,uint clusterId) {
    if(capacity==0u || capacity>GS_TEMPORAL_HASH_LIMIT ||
       (capacity&(capacity-1u))!=0u) return false;
    uint base=MeshletHistoryHash(slot,gen,clusterId)&(capacity-1u);
    [loop] for(uint step=0u;step<GS_TEMPORAL_PROBES;++step) {
        const GsTemporalEntry e=history.Get().entries[(base+step)&(capacity-1u)];
        if(e.state==0u) return false; // completed table: insertion stops at first empty
        if(e.state==2u && e.slot==slot &&
           e.generation==gen && e.clusterId==clusterId)
            return true;
    }
    return false;
}
void MeshletHistoryRecord(GPU_PTR(GsTemporalHistory) history,uint capacity,
                          uint slot,uint gen,uint clusterId) {
    if(capacity==0u || capacity>GS_TEMPORAL_HASH_LIMIT ||
       (capacity&(capacity-1u))!=0u) return;
    uint base=MeshletHistoryHash(slot,gen,clusterId)&(capacity-1u);
    [loop] for(uint step=0u;step<GS_TEMPORAL_PROBES;++step) {
        const uint at=(base+step)&(capacity-1u);
        uint previous=0u;
        // 0=empty, 1=being initialized, 2=ready. No spinning on state=1:
        // another lane may be the initializer in the SAME warp.
        InterlockedCompareExchange(history.Get().entries[at].state,0u,1u,previous);
        if(previous==0u) {
            history.Get().entries[at].slot=slot;
            history.Get().entries[at].generation=gen;
            history.Get().entries[at].clusterId=clusterId;
            DeviceMemoryBarrier();
            uint ignored;
            InterlockedExchange(history.Get().entries[at].state, 2u, ignored);
            return;
        }
        // Duplicate complete keys are okay; concurrent reservations of the
        // same key may occupy multiple slots but never cause false matches.
        if(previous==2u &&
           history.Get().entries[at].slot==slot &&
           history.Get().entries[at].generation==gen &&
           history.Get().entries[at].clusterId==clusterId) return;
    }
    InterlockedOr(history.Get().overflow,1u);
}
