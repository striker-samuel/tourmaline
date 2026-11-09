// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// GPU Scene indexed meshlet draw builder. Same 4 PSO bins and work identity
// as gpu_scene_raster_work_drawbuild, with 32 bounded lanes per GPU work row.
// Meshes with >32 clusters retain the whole-submesh indexed fallback in lane
// zero. All commands (including unused slots) are written by this dispatch.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GS_MAX_WORK 1048576u
#define GS_MAX_COMMANDS (4u * GS_MAX_WORK)
#define GS_MAX_GEO 65535u
#define GS_MAX_CANDIDATES 1048576u
#define GS_MAX_ROWS 4194304u
#define GS_MESHLET_LANES 32u
#define GS_INVALID 0xffffffffu

struct SceneSubmeshWork { uint4 identity; uint4 detail; };
struct RasterIndexedGeometry {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint flags;
};
struct RasterIndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct WorkBuffer { SceneSubmeshWork rows[GS_MAX_WORK]; };
struct OffsetBuffer { uint values[GS_MAX_CANDIDATES + 1u]; };
struct GeometryBuffer { RasterIndexedGeometry rows[GS_MAX_GEO]; };
struct IndirectBuffer { RasterIndirectCmd rows[GS_MAX_COMMANDS]; };
struct AliasBuffer { uint slots[GS_MAX_GEO]; };
struct GfxMeshEntry { uint clusterStart; uint clusterCount; };
struct GfxMeshBuffer { GfxMeshEntry rows[GS_MAX_GEO]; };
// Mirror gfx::ClusterConstants (64 bytes). Stored at each cluster's original
// registration index, with positions and radii in the mesh's LOCAL space.
struct GfxClusterData { uint4 a; float4 b; float4 c; float4 d; };
struct GfxClusterBuffer { GfxClusterData rows[GS_MAX_ROWS]; };
struct SceneInstances { GpuSceneInstanceRow rows[GS_MAX_ROWS]; };
struct CullView { DoubleFloat3 origin; float4 inwardPlanes[6]; };

struct RasterMeshletPush {
    GPU_PTR_READ(WorkBuffer) work;
    GPU_PTR_READ(OffsetBuffer) workOffsets;
    GPU_PTR_READ(GeometryBuffer) indexedGeometry;
    GPU_PTR_WRITE(IndirectBuffer) indirect;
    uint candidateCount;
    uint geometryCount;
    uint commandCapacity;
    uint binStride;
    GPU_PTR_READ(AliasBuffer) aliases;
    uint aliasCount;
    uint padding;
    GPU_PTR_READ(GfxMeshBuffer) meshes;
    GPU_PTR_READ(GfxClusterBuffer) clusters;
    GPU_PTR_READ(SceneInstances) instances;
    GPU_PTR_READ(CullView) cullView;
    uint meshCount;
    uint clusterCount;
    uint indexBase;
    uint instanceSlots;
};
GPU_PUSH(RasterMeshletPush, pc);

bool ClusterVisible(float4 sphere, GpuSceneInstance inst, CullView view) {
    // Zero/invalid radius is an unknown bound, NEVER an invisible meshlet.
    if (!all(isfinite(sphere)) || sphere.w <= 0.0f)
        return true;
    float3 x = inst.basisX.xyz;
    float3 y = inst.basisY.xyz;
    float3 z = inst.basisZ.xyz;
    float3 center = DoubleFloatRelative(inst.worldPosition, view.origin)
                  + x * sphere.x + y * sphere.y + z * sphere.z;
    float radius = sphere.w * sqrt(max(dot(x,x) + dot(y,y) + dot(z,z), 0.0f));
    if (!all(isfinite(center)) || !isfinite(radius))
        return true;
    [unroll]
    for (uint n = 0u; n < 6u; ++n) {
        float4 p = view.inwardPlanes[n];
        if (!all(isfinite(p))) return true;
        float d = dot(p.xyz, center) + p.w;
        if (!isfinite(d)) return true;
        float slack = 0.02f + 2e-6f * (abs(d) + radius + 1.0f);
        if (d < -(radius + slack)) return false;
    }
    return true;
}

[numthreads(128, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.commandCapacity || i >= GS_MAX_COMMANDS)
        return;
    RasterIndirectCmd cmd = (RasterIndirectCmd)0;
    pc.indirect.Get().rows[i] = cmd; // full initialization on every path
    const uint stride = pc.binStride;
    if (stride == 0u || stride > GS_MAX_WORK ||
        (stride % GS_MESHLET_LANES) != 0u ||
        pc.commandCapacity != 4u * stride ||
        pc.candidateCount > GS_MAX_CANDIDATES)
        return;
    const uint bin = i / stride;
    const uint slot = (i % stride) / GS_MESHLET_LANES;
    const uint lane = (i % stride) % GS_MESHLET_LANES;
    const uint count = pc.workOffsets.Get().values[pc.candidateCount];
    if (count > stride / GS_MESHLET_LANES || slot >= count)
        return;
    const SceneSubmeshWork work = pc.work.Get().rows[slot];
    const uint transientId = work.identity.z;
    if (work.identity.x == GS_INVALID || work.identity.y == 0u ||
        work.detail.z != bin || transientId >= pc.aliasCount ||
        transientId >= pc.meshCount || transientId >= GS_MAX_GEO)
        return;
    uint geometry = pc.aliases.Get().slots[transientId];
    if (geometry >= pc.geometryCount || geometry >= GS_MAX_GEO)
        return;
    const RasterIndexedGeometry indexed = pc.indexedGeometry.Get().rows[geometry];
    if (indexed.indexCount == 0u)
        return;
    const GfxMeshEntry mesh = pc.meshes.Get().rows[transientId];
    // No partial meshlet draw set: a mesh above the lane budget is still
    // rendered whole (lane zero), not truncated.
    const bool fullDraw = mesh.clusterCount == 0u ||
                          mesh.clusterCount > GS_MESHLET_LANES ||
                          mesh.clusterStart > pc.clusterCount ||
                          mesh.clusterCount > pc.clusterCount - mesh.clusterStart;
    if (fullDraw) {
        if (lane != 0u) return;
        cmd.indexCount = indexed.indexCount;
        cmd.instanceCount = 1u;
        cmd.firstIndex = indexed.firstIndex;
        cmd.vertexOffset = indexed.vertexOffset;
        cmd.firstInstance = slot;
        pc.indirect.Get().rows[i] = cmd;
        return;
    }
    if (lane >= mesh.clusterCount) return;
    const GfxClusterData cluster =
        pc.clusters.Get().rows[mesh.clusterStart + lane];
    if (cluster.a.y == 0u || cluster.a.y > 0x55555555u ||
        cluster.a.x > 0xffffffffu - pc.indexBase)
        return; // registered indexed geometry was validated at publication
    const uint first = cluster.a.x + pc.indexBase;
    const uint triangles = cluster.a.y * 3u;
    if (first < indexed.firstIndex ||
        triangles > indexed.indexCount ||
        first - indexed.firstIndex > indexed.indexCount - triangles)
        return;
    // Invalid identities do not draw; unknown bounds keep the cluster.
    if (work.identity.x >= min(pc.instanceSlots, GS_MAX_ROWS)) return;
    const GpuSceneInstanceRow row = pc.instances.Get().rows[work.identity.x];
    if (row.identity.y != 1u || row.identity.x != work.identity.y) return;
    if (!ClusterVisible(cluster.b, row.value, pc.cullView.Get()))
        return;
    cmd.indexCount = triangles;
    cmd.instanceCount = 1u;
    cmd.firstIndex = first;
    cmd.vertexOffset = indexed.vertexOffset;
    cmd.firstInstance = slot;
    pc.indirect.Get().rows[i] = cmd;
}
