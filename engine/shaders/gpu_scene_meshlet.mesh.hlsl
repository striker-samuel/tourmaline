// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Mesh-stage consumer for gpu_scene_meshlet.task.hlsl.
// One mesh workgroup per surviving cluster. The existing filtered geometry
// index stream contains global vertex indices; the indexed geometry's signed
// vertexOffset is applied exactly as in vkCmdDrawIndexedIndirect.
// Conservative v1 emits 3 vertices per triangle (<=64 tris, <=192 verts).
// The producer MUST validate all cluster index ranges/vertex IDs beforehand.
#define GFXMESH_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "gpu_scene_task_mesh.hlsli"

GPU_PUSH(GsTaskMeshPush, pc);

struct GsMeshVertex {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    [[vk::location(9)]] nointerpolation uint userData : TEXCOORD8;
    [[vk::location(10)]] nointerpolation uint lightLayerMask : TEXCOORD9;
};

GsMeshVertex FetchVertex(uint vertexId,
                         GpuSceneInstance inst,
                         GsRasterView view,
                         GsTaskMeshParams p) {
    GsMeshVertex v=(GsMeshVertex)0;
    const uint pb=vertexId*3u, sb=vertexId*4u;
    const float3 local=float3(
        asfloat(p.positions.Get().word[pb]),
        asfloat(p.positions.Get().word[pb+1u]),
        asfloat(p.positions.Get().word[pb+2u]));
    const uint color=p.shading.Get().word[sb];
    const uint uv=p.shading.Get().word[sb+1u];
    const uint nword=p.shading.Get().word[sb+2u];
    const uint tword=p.shading.Get().word[sb+3u];

    const float3 a=inst.basisX.xyz;
    const float3 b=inst.basisY.xyz;
    const float3 c=inst.basisZ.xyz;
    const float3 relative=DoubleFloatRelative(inst.worldPosition,view.origin)
        +a*local.x+b*local.y+c*local.z;
    v.pos=mul(view.relativeViewProjection,float4(relative,1.0f));
    v.worldPos=relative+view.origin.high.xyz+view.origin.low.xyz;
    v.color=float4(color&255u,(color>>8u)&255u,
                   (color>>16u)&255u,(color>>24u)&255u)*(1.0f/255.0f);
    v.uv=float2(f16tof32(uv&65535u),f16tof32(uv>>16u));
    const float3 normal=OctDecodeNormal(nword);
    const float4 tangent=OctDecodeTangent(tword);
    const float3 cx=cross(b,c),cy=cross(c,a),cz=cross(a,b);
    const float det=dot(a,cx);
    const float3 worldN=abs(det)>1e-12f
        ?(cx*normal.x+cy*normal.y+cz*normal.z)/det
        :a*normal.x+b*normal.y+c*normal.z;
    const float3 worldT=a*tangent.x+b*tangent.y+c*tangent.z;
    v.normal=normalize(worldN);
    v.tangent=float4(normalize(worldT),
                     tangent.w*(det<0.0f?-1.0f:1.0f));
    v.lightmapUv=0.0f;
    v.lmChunk=0xffffffffu;
    v.meshMaterial=inst.refs.z;
    v.ditherCloseup=0u;
    v.userData=0u;
    v.lightLayerMask=0xffffffffu;
    return v;
}

[outputtopology("triangle")]
[numthreads(32,1,1)]
void mainMS(uint3 gid : SV_GroupID,
            uint3 tid : SV_GroupThreadID,
            in payload GsTaskPayload task,
            out vertices GsMeshVertex verts[GS_MESH_MAX_VERTS],
            out indices uint3 tris[GS_MESH_MAX_TRIS]) {
    const GsTaskMeshParams p=pc.params.Get();
    bool valid=gid.x<task.surviving && gid.x<GS_TASK_THREADS &&
        task.workSlot<p.workCount && task.workSlot<GS_MAX_WORK;
    GsWorkRow work=(GsWorkRow)0;
    GpuSceneInstance inst=(GpuSceneInstance)0;
    GsCluster c=(GsCluster)0;
    GsIndexedGeometry geo=(GsIndexedGeometry)0;
    if (valid) {
        work=p.work.Get().rows[task.workSlot];
        valid=work.identity.y!=0u && work.identity.x<p.instanceCount &&
            work.identity.x<GS_MAX_INSTANCES &&
            work.detail.z==pc.rasterClass;
    }
    if (valid) {
        const GpuSceneInstanceRow row=p.instances.Get().rows[work.identity.x];
        valid=row.identity.y==1u && row.identity.x==work.identity.y;
        if (valid) inst=row.value;
    }
    if (valid) {
        const uint geometryId=work.identity.z;
        valid=geometryId<p.aliasCount && geometryId<GS_MAX_GEO;
        if (valid) {
            const uint geoSlot=p.aliases.Get().slots[geometryId];
            valid=geoSlot<p.geometryCount && geoSlot<GS_MAX_GEO;
            if (valid) geo=p.geometry.Get().rows[geoSlot];
        }
    }
    if (valid) {
        const uint clusterId=task.clusterIds[gid.x];
        valid=clusterId<p.clusterCount && clusterId<GS_MAX_CLUSTERS;
        if (valid) c=p.clusters.Get().rows[clusterId];
    }
    // Cluster indexCount and its complete range MUST have been validated
    // before task ownership. This guard is defense in depth, not fallback.
    uint triCount=valid?c.a.y:0u;
    if (triCount>GS_MESH_MAX_TRIS ||
        c.a.x>0xffffffffu-p.indexBase)
        triCount=0u;
    const uint first=c.a.x+p.indexBase;
    if (first>p.indexWordCapacity ||
        triCount> (p.indexWordCapacity-first)/3u ||
        first<geo.firstIndex ||
        triCount>geo.indexCount/3u ||
        first-geo.firstIndex>geo.indexCount-triCount*3u)
        triCount=0u;

    SetMeshOutputCounts(triCount*3u,triCount);
    if (triCount==0u) return;

    const GsRasterView view=p.rasterView.Get();
    // Two triangles per lane. No subgroup-width assumption.
    for (uint t=tid.x;t<triCount;t+=GS_MESH_THREADS) {
        const uint firstVertex=first+t*3u;
        [unroll] for (uint k=0u;k<3u;++k) {
            const uint raw=p.indices.Get().index[firstVertex+k];
            const int64_t vertexSigned=(int64_t)raw+(int64_t)geo.vertexOffset;
            // Publication guarantees 0 <= vertexSigned < vertexCapacity.
            // Malformed topology cannot be recovered by dropping single
            // primitives here: all ranges must be checked on the CPU/GPU
            // producer before assigning mesh ownership.
            const uint vertexId=(uint)vertexSigned;
            verts[t*3u+k]=FetchVertex(vertexId,inst,view,p);
        }
        tris[t]=uint3(t*3u,t*3u+1u,t*3u+2u);
    }
}
