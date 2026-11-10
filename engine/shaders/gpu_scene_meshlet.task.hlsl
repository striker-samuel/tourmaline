// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU Scene meshlet visibility in the task/amplification stage.
// Each input task group covers <=32 meshlets of one work record.
// The producer MUST route invalid topology to indexed fallback; the
// task shader only performs conservative visibility rejection.
// No global atomic append, no per-frame dense meshlet candidate stream.
#define GPU_SCENE_RASTER_HIZCULL_PASS // common.hlsli: declare MaterialsBuffer, skip the scene pc
#include "common.hlsli"
#include "gpu_scene_task_mesh.hlsli"
#include "gfxfilter.hlsli" // GfxHzbCtx / ProjectBound / BoundHzbOccluded (meshlet Hi-Z)

GPU_PUSH(GsTaskMeshPush, pc);
groupshared uint gsScan[GS_TASK_THREADS];
// The task payload is a groupshared object: DXC's SPIR-V backend maps a
// groupshared variable handed to DispatchMesh() to
// TaskPayloadWorkgroupEXT (the `in payload` parameter form only works on
// the DXIL path and rejects the DispatchMesh argument here). The mesh
// stage reads the same object through its `in payload` parameter.
groupshared GsTaskPayload gsPayload;

bool SphereVisible(float4 sphere, GpuSceneInstance inst, GsView view) {
    if (!all(isfinite(sphere)) || sphere.w <= 0.0f) return true;
    const float3 x=inst.basisX.xyz, y=inst.basisY.xyz, z=inst.basisZ.xyz;
    const float3 center=DoubleFloatRelative(inst.worldPosition,view.origin)
        + x*sphere.x + y*sphere.y + z*sphere.z;
    const float radius=sphere.w*sqrt(dot(x,x)+dot(y,y)+dot(z,z));
    if (!all(isfinite(center)) || !isfinite(radius)) return true;
    [unroll] for (uint p=0u;p<6u;++p) {
        const float4 plane=view.planes[p];
        if (!all(isfinite(plane))) return true;
        const float distance=dot(plane.xyz,center)+plane.w;
        if (!isfinite(distance)) return true;
        const float slack=0.02f+2e-6f*(abs(distance)+radius+1.0f);
        if (distance<-(radius+slack)) return false;
    }
    return true;
}

bool ConeCulled(GsCluster c, GpuSceneInstance inst, GsView view) {
    if (!all(isfinite(c.c)) || !all(isfinite(c.d)) ||
        c.c.w>=1.0f || c.c.w< -1.0f) return false;
    const float3 x=inst.basisX.xyz,y=inst.basisY.xyz,z=inst.basisZ.xyz;
    const float3 a=cross(y,z),b=cross(z,x),d=cross(x,y);
    const float det=dot(x,a);
    if (!isfinite(det)||abs(det)<1e-12f) return false;
    const float3 axis=(a*c.d.x+b*c.d.y+d*c.d.z)/det;
    const float3 apex=DoubleFloatRelative(inst.worldPosition,view.origin)
        +x*c.c.x+y*c.c.y+z*c.c.z;
    const float al=dot(axis,axis),pl=dot(apex,apex);
    if (!isfinite(al)||!isfinite(pl)||al<1e-12f||pl<1e-12f) return false;
    return dot(axis*rsqrt(al),apex*rsqrt(pl))>=c.c.w;
}

// Meshlet-level reversed-Z MIN Hi-Z occlusion: project the cluster bound
// sphere (object space, xyz center / w radius) by the instance model and
// reject it when its nearest depth sits behind every covered Hi-Z texel.
// Mirrors the gfx filter's per-cluster ClusterOccluded, applied at meshlet
// granularity (the missing piece vs Niagara's task-shader occlusion).
bool MeshletOccluded(float4 bound, GpuSceneInstance inst, GsTaskMeshParams p) {
    if (!all(isfinite(bound)) || bound.w <= 0.0f || p.hizMips == 0u ||
        p.hizWidth == 0u || p.hizHeight == 0u)
        return false;
    const GsRasterView rv = p.rasterView.Get();
    const float3 originRelative =
        DoubleFloatRelative(inst.worldPosition, rv.origin);
    const float3 x = inst.basisX.xyz;
    const float3 y = inst.basisY.xyz;
    const float3 z = inst.basisZ.xyz;
    const float4x4 model = float4x4(
        x.x, y.x, z.x, originRelative.x,
        x.y, y.y, z.y, originRelative.y,
        x.z, y.z, z.z, originRelative.z,
        0.0f, 0.0f, 0.0f, 1.0f);
    GfxHzbCtx ctx;
    ctx.viewProj = rv.relativeViewProjection;
    ctx.eye = float3(0.0f, 0.0f, 0.0f);
    ctx.width = p.hizWidth;
    ctx.height = p.hizHeight;
    ctx.mips = p.hizMips;
    ctx.hzb = p.hiz;
    const BoundProjection bp = ProjectBound(bound, model, ctx);
    return bp.testable && bp.onTarget && BoundHzbOccluded(bp, ctx);
}

[shader("amplification")]
[numthreads(32,1,1)]
void mainAS(uint3 groupId : SV_GroupID,
            uint3 threadId : SV_GroupThreadID) {
    const uint lane=threadId.x;
    const uint groupIndex=groupId.x+groupId.y*pc.taskGridX;
    const GsTaskMeshParams p=pc.params.Get();

    GsTaskGroup group=(GsTaskGroup)0;
    GsWorkRow work=(GsWorkRow)0;
    GpuSceneInstance inst=(GpuSceneInstance)0;
    const uint liveGroups=(p.workCapacity<=GS_MAX_WORK)
        ? p.taskOffsets.Get().values[p.workCapacity] : 0u;
    bool valid=pc.taskGridX!=0u && groupId.x<pc.taskGridX &&
        groupIndex<pc.groupCount && groupIndex<liveGroups &&
        liveGroups<=GS_MAX_TASK_GROUPS && pc.groupBase<=GS_MAX_TASK_GROUPS &&
        groupIndex<GS_MAX_TASK_GROUPS-pc.groupBase;

    if (valid) {
        group=p.groups.Get().rows[pc.groupBase+groupIndex];
        valid=group.workSlot<p.workCount && group.workSlot<GS_MAX_WORK &&
            group.clusterCount>0u && group.clusterCount<=GS_TASK_THREADS &&
            group.rasterClass==pc.rasterClass &&
            group.firstCluster<=p.clusterCount &&
            group.clusterCount<=p.clusterCount-group.firstCluster;
    }
    if (valid) {
        work=p.work.Get().rows[group.workSlot];
        valid=work.identity.y!=0u && work.identity.x<p.instanceCount &&
            work.identity.x<GS_MAX_INSTANCES &&
            work.detail.z==group.rasterClass;
        if (valid) {
            const GpuSceneInstanceRow row=p.instances.Get().rows[work.identity.x];
            valid=row.identity.y==1u && row.identity.x==work.identity.y;
            if (valid) inst=row.value;
        }
    }

    bool keep=false;
    uint clusterId=0u;
    if (valid && lane<group.clusterCount) {
        clusterId=group.firstCluster+lane;
        keep=true;
        if (p.viewValid!=0u) {
            const GsView view=p.cullView.Get();
            const GsCluster c=p.clusters.Get().rows[clusterId];
            keep=SphereVisible(c.b,inst,view);
            if (keep && p.coneEnabled!=0u &&
                (group.rasterClass&1u)==0u)
                keep=!ConeCulled(c,inst,view);
            if (keep && p.hizValid!=0u)
                keep=!MeshletOccluded(c.b,inst,p);
        }
    }

    // Workgroup-wide exclusive prefix; unlike WavePrefixCountBits this
    // works on both 32- and 64-lane subgroup hardware. No atomics.
    gsScan[lane]=keep?1u:0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint step=1u;step<GS_TASK_THREADS;step<<=1u) {
        const uint add=lane>=step?gsScan[lane-step]:0u;
        GroupMemoryBarrierWithGroupSync();
        gsScan[lane]+=add;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint rank=gsScan[lane]-(keep?1u:0u);
    const uint survivorCount=gsScan[GS_TASK_THREADS-1u];
    if (lane==0u) {
        gsPayload.workSlot=group.workSlot;
        gsPayload.surviving=survivorCount;
    }
    if (keep)
        gsPayload.clusterIds[rank]=clusterId;

    GroupMemoryBarrierWithGroupSync();
    // DispatchMesh must be executed uniformly by the entire task group.
    DispatchMesh(survivorCount,1u,1u,gsPayload);
}
