// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Mesh-stage consumer for gpu_scene_meshlet.task.hlsl.
// One mesh workgroup per surviving cluster. Indexed-meshlet output: each
// unique local vertex is emitted once and referenced through local triangle
// ids (3 packed u8 per uint, one uint per triangle), so a <=64-tri meshlet
// outputs <=64 vertices instead of 3*64. The producer MUST validate all
// cluster vertex/triangle ranges beforehand (guards are defense in depth).
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

// Per-INSTANCE transform frame, hoisted out of the per-vertex FetchVertex
// loop. The basis columns + their cofactors (the inverse-transpose basis) and
// the determinant are instance-uniform: recomputing 3 cross + 1 dot per vertex
// was pure waste, and the determinant is also the rigid-transform test for the
// normalize elision below.
struct GsInstanceFrame {
    float3 a, b, c;     // basis columns (inst.basisX/Y/Z)
    float3 cx, cy, cz;  // cofactor basis (cross(b,c), cross(c,a), cross(a,b))
    float det;          // dot(a, cross(b,c))
    float3 originRelative;      // DoubleFloatRelative(inst.worldPosition, view.origin)
    float4x4 viewProj;          // relativeViewProjection
    float3 originHigh;          // view.origin.high.xyz
    float3 originLow;           // view.origin.low.xyz
    uint meshMaterial;          // inst.refs.z
};

GsInstanceFrame MakeInstanceFrame(GpuSceneInstance inst, GsRasterView view) {
    GsInstanceFrame f;
    f.a = inst.basisX.xyz;
    f.b = inst.basisY.xyz;
    f.c = inst.basisZ.xyz;
    f.cx = cross(f.b, f.c);
    f.cy = cross(f.c, f.a);
    f.cz = cross(f.a, f.b);
    f.det = dot(f.a, f.cx);
    f.originRelative = DoubleFloatRelative(inst.worldPosition, view.origin);
    f.viewProj = view.relativeViewProjection;
    f.originHigh = view.origin.high.xyz;
    f.originLow = view.origin.low.xyz;
    f.meshMaterial = inst.refs.z;
    return f;
}

GsMeshVertex FetchVertex(uint vertexId,
                         GsInstanceFrame f,
                         GsTaskMeshParams p,
                         bool fastNormal) {
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

    const float3 relative=f.originRelative
        +f.a*local.x+f.b*local.y+f.c*local.z;
    v.pos=mul(f.viewProj,float4(relative,1.0f));
    v.worldPos=relative+f.originHigh+f.originLow;
    v.color=float4(color&255u,(color>>8u)&255u,
                   (color>>16u)&255u,(color>>24u)&255u)*(1.0f/255.0f);
    v.uv=float2(f16tof32(uv&65535u),f16tof32(uv>>16u));
    const float3 normal=OctDecodeNormal(nword);
    const float4 tangent=OctDecodeTangent(tword);
    const float3 worldN=abs(f.det)>1e-12f
        ?(f.cx*normal.x+f.cy*normal.y+f.cz*normal.z)/f.det
        :f.a*normal.x+f.b*normal.y+f.c*normal.z;
    const float3 worldT=f.a*tangent.x+f.b*tangent.y+f.c*tangent.z;
    // Rigid-transform elision (opt-in): det == 1 means the instance transform
    // is a pure rotation, so the inverse-transpose and the tangent transform
    // both preserve length and the oct-decode already returned a UNIT vector —
    // the two normalize() are dead math. det is instance-uniform, so this
    // branch is uniform across the meshlet (no divergence).
    const bool rigid = fastNormal && abs(f.det - 1.0f) < 1e-4f;
    v.normal = rigid ? worldN : normalize(worldN);
    v.tangent = float4(rigid ? worldT : normalize(worldT),
                       tangent.w*(f.det<0.0f?-1.0f:1.0f));
    v.lightmapUv=0.0f;
    v.lmChunk=0xffffffffu;
    v.meshMaterial=f.meshMaterial;
    v.ditherCloseup=0u;
    v.userData=0u;
    v.lightLayerMask=0xffffffffu;
    return v;
}

// GPU-Scene-native debug color (debugMode 1 = meshlet hash hue, mirroring
// GfxRenderer::DebugClusterColor mode 0): stable golden-ratio hash of the
// GLOBAL cluster id -> HSV hue -> RGB. Authored in display space, so the
// caller pre-scales by the debug-view gain (1/kDebugViewExposure) to survive
// the HDR + ACES chain exactly like the direct meshdebug path.
float3 MeshletDebugColor(uint clusterId) {
    uint h = clusterId * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    const float hue = float(h % 1024u) / 1024.0f;
    const float s = 0.85f;
    const float v = 0.95f;
    const float c = v * s;
    const float hh = hue * 6.0f;
    const float x = c * (1.0f - abs(fmod(hh, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hh < 1.0f) { r = c; g = x; }
    else if (hh < 2.0f) { r = x; g = c; }
    else if (hh < 3.0f) { g = c; b = x; }
    else if (hh < 4.0f) { g = x; b = c; }
    else if (hh < 5.0f) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = v - c;
    return float3(r + m, g + m, b + m);
}

// LOD palette (mirrors world_renderer.cpp kLodPalette): green LOD0 -> yellow
// LOD1 -> orange LOD2 -> red LOD3 -> magenta LOD4+. Authored display colors.
float3 LodPaletteColor(uint lod) {
    if (lod == 0u) return float3(0.20f, 0.90f, 0.30f);
    if (lod == 1u) return float3(0.95f, 0.85f, 0.20f);
    if (lod == 2u) return float3(1.00f, 0.55f, 0.15f);
    if (lod == 3u) return float3(1.00f, 0.20f, 0.20f);
    return float3(1.00f, 0.20f, 1.00f);
}

// Cone heat (mirrors GfxRenderer::DebugClusterColor mode 1): cutoff ~1
// (degenerate/wide, never culls) -> red; tight useful cones -> green/cyan.
// Reuses the cluster's baked cone metadata (c.c.w = coneCutoff), no extra
// GPU buffer.
float3 ConeHeatColor(float cutoff) {
    const float t = clamp((cutoff + 1.0f) * 0.5f, 0.0f, 1.0f);
    return float3(t, 1.0f - abs(t - 0.5f), 1.0f - t);
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
    // Debug-view flat color (debugMode 1 = per-meshlet hash hue). Authored in
    // display space; pre-scaled by 1/kDebugViewExposure so the HDR + ACES
    // chain reproduces it (the same convention as the direct meshdebug path).
    float4 dbgColor=float4(0.0f,0.0f,0.0f,1.0f);
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
        if (valid) {
            c=p.clusters.Get().rows[clusterId];
            if (p.debugMode==1u)
                dbgColor=float4(MeshletDebugColor(clusterId) * 1000.0f, 1.0f);
            else if (p.debugMode==2u)
                dbgColor=float4(LodPaletteColor(work.detail.x) * 1000.0f, 1.0f);
            else if (p.debugMode==3u)
                dbgColor=float4(ConeHeatColor(c.c.w) * 1000.0f, 1.0f);
        }
    }
    // Indexed-meshlet output: unique local vertices (a.w count at a.z) +
    // local triangle ids (a.y count at e.x). Range guards are defense in
    // depth; the producer validates topology before mesh ownership.
    uint vertexCount=valid?c.a.w:0u;
    uint triCount=valid?c.a.y:0u;
    if (vertexCount>GS_MESH_MAX_VERTS || triCount>GS_MESH_MAX_TRIS ||
        c.a.z>p.vertexCapacity || vertexCount>p.vertexCapacity-c.a.z ||
        c.e.x>p.indexWordCapacity/3u || triCount>p.indexWordCapacity/3u-c.e.x) {
        vertexCount=0u;
        triCount=0u;
    }
    SetMeshOutputCounts(vertexCount,triCount);
    if (vertexCount==0u || triCount==0u) return;

    const GsRasterView view=p.rasterView.Get();
    const GsInstanceFrame frame=MakeInstanceFrame(inst,view);
    const bool fastNormal=(pc.vertexOptFlags & 1u) != 0u;
    const uint vertexBase=c.a.z;
    const uint triBase=c.e.x;
    // One unique vertex per lane. The meshlet vertex stream is already
    // heap-global (lifted at registration); only the geometry's signed
    // vertexOffset is applied, mirroring vkCmdDrawIndexedIndirect.
    for (uint v=tid.x;v<vertexCount;v+=GS_MESH_THREADS) {
        const uint globalId=p.meshletVertices.Get().id[vertexBase+v];
        const int64_t vertexSigned=(int64_t)globalId+(int64_t)geo.vertexOffset;
        verts[v]=FetchVertex((uint)vertexSigned,frame,p,fastNormal);
        if (p.debugMode!=0u)
            verts[v].color=dbgColor;
    }
    // One local triangle per lane (3 packed u8 ids).
    for (uint t=tid.x;t<triCount;t+=GS_MESH_THREADS) {
        const uint packed=p.meshletTriangles.Get().tri[triBase+t];
        tris[t]=uint3(packed&255u,(packed>>8u)&255u,(packed>>16u)&255u);
    }
}
