// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Meshlet expansion stage 3: write one DENSE candidate per true work/cluster,
// after the stock GPU work-prefix block/groups/add scan. No PSO x lane array.
// Dispatch *indirectly* using gpu_scene_meshlet_dispatch.cs.hlsl.
// Invalid/cull candidates carry class INVALID; #301-style PSO count/scan/
// scatter packs only surviving records into indexed commands.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"
#define MAX_WORK 1048576u
#define MAX_CANDIDATES 1048576u
#define MAX_GEO 65535u
#define MAX_CLUSTERS 4194304u
#define INVALID 0xffffffffu
struct WorkRow { uint4 identity; uint4 detail; };
struct Work { WorkRow rows[MAX_WORK]; };
struct Offset { uint values[MAX_WORK+1u]; };
struct SceneOffsets { uint values[MAX_WORK+1u]; };
struct Mesh { uint first; uint count; };
struct Meshes { Mesh rows[MAX_GEO]; };
struct Indexed { uint count; uint first; int vertexOffset; uint flags; };
struct Geometry { Indexed rows[MAX_GEO]; };
struct Aliases { uint slots[MAX_GEO]; };
struct Cluster { uint4 a; float4 b; float4 c; float4 d; };
struct Clusters { Cluster rows[MAX_CLUSTERS]; };
struct Instances { GpuSceneInstanceRow rows[MAX_CLUSTERS]; };
struct View { DoubleFloat3 origin; float4 planes[6]; };
// 32-byte candidate contract: draw = (firstIndex,indexCount,vertexOffsetBits,
// originalWorkSlot); meta = (PSO class or INVALID,clusterId,flags,reserved).
struct Candidate { uint4 draw; uint4 meta; };
struct Candidates { Candidate rows[MAX_CANDIDATES]; };
struct Push {
    GPU_PTR_READ(Work) work;
    GPU_PTR_READ(SceneOffsets) sceneWorkOffsets;
    GPU_PTR_READ(Offset) meshletOffsets;
    GPU_PTR_READ(Meshes) meshes;
    GPU_PTR_READ(Geometry) geometry;
    GPU_PTR_READ(Aliases) aliases;
    GPU_PTR_READ(Clusters) clusters;
    GPU_PTR_READ(Instances) instances;
    GPU_PTR_READ(View) view;
    GPU_PTR_WRITE(Candidates) candidates;
    uint sceneCandidateCount;
    uint workCapacity;
    uint meshCount;
    uint geometryCount;
    uint aliasCount;
    uint clusterCount;
    uint instanceCount;
    uint candidateCapacity;
    uint indexBase;
    uint viewValid;
    uint coneEnabled;
    uint reserved;
};
GPU_PUSH(Push, pc);
bool SphereVisible(float4 s, GpuSceneInstance inst, View view) {
    if (!all(isfinite(s)) || s.w <= 0.0f) return true;
    const float3 x=inst.basisX.xyz, y=inst.basisY.xyz, z=inst.basisZ.xyz;
    const float3 p = DoubleFloatRelative(inst.worldPosition, view.origin) +
                     x*s.x + y*s.y + z*s.z;
    const float r = s.w * sqrt(dot(x,x)+dot(y,y)+dot(z,z));
    if (!all(isfinite(p)) || !isfinite(r)) return true;
    [unroll] for (uint i=0u; i<6u; ++i) {
        const float4 plane = view.planes[i];
        if (!all(isfinite(plane))) return true;
        const float distance=dot(plane.xyz,p)+plane.w;
        if (!isfinite(distance)) return true;
        const float slack = 0.02f + 2e-6f*(abs(distance)+r+1.0f);
        if (distance < -(r+slack)) return false;
    }
    return true;
}
bool ConeCulled(Cluster c, GpuSceneInstance inst, View view) {
    if (!all(isfinite(c.c)) || !all(isfinite(c.d)) ||
        c.c.w >= 1.0f || c.c.w < -1.0f) return false;
    float3 x=inst.basisX.xyz, y=inst.basisY.xyz, z=inst.basisZ.xyz;
    float3 a=cross(y,z), b=cross(z,x), d=cross(x,y);
    float det=dot(x,a);
    if (!isfinite(det) || abs(det)<1e-12f) return false;
    float3 axis=(a*c.d.x+b*c.d.y+d*c.d.z)/det;
    float3 apex=DoubleFloatRelative(inst.worldPosition,view.origin)+
                x*c.c.x+y*c.c.y+z*c.c.z;
    float al=dot(axis,axis), pl=dot(apex,apex);
    if (!isfinite(al)||!isfinite(pl)||al<1e-12f||pl<1e-12f) return false;
    return dot(axis*rsqrt(al),apex*rsqrt(pl))>=c.c.w;
}
[numthreads(128,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i=tid.x;
    if (pc.sceneCandidateCount>MAX_WORK || pc.workCapacity>MAX_WORK) return;
    const uint workCount=pc.sceneWorkOffsets.Get().values[pc.sceneCandidateCount];
    if (workCount>pc.workCapacity) return;
    const uint total=pc.meshletOffsets.Get().values[workCount];
    if (total>min(pc.candidateCapacity,MAX_CANDIDATES)||i>=total) return;
    Candidate candidate=(Candidate)0;
    candidate.meta.x=INVALID;
    candidate.meta.y=INVALID;
    pc.candidates.Get().rows[i]=candidate;
    // Monotonic prefix may contain repeated values for zero-count work.
    // upper_bound(offsets,i)-1 obtains the owning original work slot.
    uint lo=0u,hi=workCount;
    while (lo<hi) {
        const uint mid=lo+(hi-lo+1u)/2u;
        if (pc.meshletOffsets.Get().values[mid]<=i) lo=mid;
        else hi=mid-1u;
    }
    if (lo>=workCount) return;
    const uint slot=lo, lane=i-pc.meshletOffsets.Get().values[slot];
    const WorkRow w=pc.work.Get().rows[slot];
    const uint id=w.identity.z;
    if (w.identity.y==0u || w.identity.x>=min(pc.instanceCount,MAX_CLUSTERS) ||
        w.detail.z>=4u || id>=min(pc.meshCount,MAX_GEO) ||
        id>=pc.aliasCount) return;
    const uint geo=pc.aliases.Get().slots[id];
    if (geo>=min(pc.geometryCount,MAX_GEO)) return;
    const Indexed g=pc.geometry.Get().rows[geo];
    if (g.count==0u) return;
    const GpuSceneInstanceRow row=pc.instances.Get().rows[w.identity.x];
    if (row.identity.y!=1u || row.identity.x!=w.identity.y) return;
    const Mesh m=pc.meshes.Get().rows[id];
    const bool hasMeshlets=m.count!=0u&&m.count<=1024u&&
                           m.first<=pc.clusterCount&&m.count<=pc.clusterCount-m.first;
    const bool fallback=!hasMeshlets;
    uint first=g.first, count=g.count,clusterId=INVALID;
    if (!fallback) {
        if (lane>=m.count) return;
        clusterId=m.first+lane;
        const Cluster c=pc.clusters.Get().rows[clusterId];
        if (c.a.y==0u || c.a.y>0x55555555u ||
            c.a.x>INVALID-pc.indexBase) {
            // Invalid meshlet metadata: fail open with a whole-submesh
            // draw. This may duplicate valid clusters but never loses a
            // triangle. Publication should validate metadata beforehand.
            clusterId=INVALID;
        } else {
            const uint f=c.a.x+pc.indexBase,n=c.a.y*3u;
            if (f<g.first||n>g.count||f-g.first>g.count-n) {
                clusterId=INVALID;
            } else {
                if (pc.viewValid!=0u) {
                    const View view=pc.view.Get();
                    if (!SphereVisible(c.b,row.value,view)) return;
                    if (pc.coneEnabled!=0u && (w.detail.z&1u)==0u &&
                        ConeCulled(c,row.value,view)) return;
                }
                first=f;count=n;
            }
        }
    }
    candidate.draw=uint4(first,count,asuint(g.vertexOffset),slot);
    candidate.meta=uint4(w.detail.z,clusterId,0u,0u);
    pc.candidates.Get().rows[i]=candidate;
}
