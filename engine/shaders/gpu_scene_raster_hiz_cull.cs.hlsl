// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// GPU-scene raster Hi-Z command gate. Runs AFTER the GPU-scene occluder
// depth pass and the reversed-Z MIN pyramid, but BEFORE Main/depth.
// The occluder depth uses the unfiltered frustum-visible commands; the
// final commands can only lose instances, never gain invalid references.
// Unknown, invalid or untestable bounds always KEEP the command.
// The pass macro + common.hlsli include follow the sibling gfxfilter.hlsli
// consumers (gfxbatchcull/gfxdrawcull/gfxcone_debug): gfxfilter.hlsli's
// GfxFilterPush references MaterialsBuffer, which only common.hlsli
// declares, and the macro keeps common.hlsli from declaring the scene-wide
// `pc` push block alongside this kernel's own RasterHizPush.
#define GPU_SCENE_RASTER_HIZCULL_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GS_RASTER_MAX_WORK 1048576u
#define GS_RASTER_MAX_COMMANDS (4u * GS_RASTER_MAX_WORK)
#define GS_RASTER_MAX_INSTANCES 4194304u

struct RasterIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct RasterCommands { RasterIndirectCommand rows[GS_RASTER_MAX_COMMANDS]; };
struct RasterWorkRow { uint4 identity; uint4 detail; };
struct RasterWork { RasterWorkRow rows[GS_RASTER_MAX_WORK]; };
struct RasterInstances { GpuSceneInstanceRow rows[GS_RASTER_MAX_INSTANCES]; };
struct RasterMeshes { GpuSceneMeshRow rows[GS_RASTER_MAX_INSTANCES]; };
struct RasterHistory { uint generations[GS_RASTER_MAX_INSTANCES]; };
// GfxFilter static mesh/cluster tables are revision-local. The command
// builder validates them; temporal Hi-Z uses the same flight's registration.
struct GfxMeshEntry { uint clusterStart; uint clusterCount; };
struct GfxMeshes { GfxMeshEntry rows[65535u]; };
struct GfxClusterData { uint4 a; float4 b; float4 c; float4 d; };
struct GfxClusters { GfxClusterData rows[GS_RASTER_MAX_INSTANCES]; };
struct RasterViewData {
    float4x4 relativeViewProjection;
    DoubleFloat3 origin;
};
struct RasterHizPush {
    GPU_PTR(RasterCommands) commands;
    GPU_PTR(RasterCommands) remainder;
    GPU_PTR_READ(RasterWork) work;
    GPU_PTR_READ(RasterInstances) instances;
    GPU_PTR_READ(RasterMeshes) meshes;
    GPU_PTR_READ(RasterViewData) view;
    GPU_PTR_READ(HzbBuffer) hiz;
    GPU_PTR_READ(RasterHistory) previous;
    uint commandCapacity;
    uint instanceSlots;
    uint meshSlots;
    uint width;
    uint height;
    uint mips;
    uint historySlots;
    uint historyValid;
    uint reserved;
    GPU_PTR_READ(GfxMeshes) gfxMeshes;
    GPU_PTR_READ(GfxClusters) gfxClusters;
    uint meshletLanes;
    uint gfxMeshCount;
    uint gfxClusterCount;
    uint meshletActive;
};
GPU_PUSH(RasterHizPush, pcH);

[numthreads(128, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pcH.commandCapacity || i >= GS_RASTER_MAX_COMMANDS ||
        pcH.reserved == 0u)
        return; // HZB culling disabled; still build the pyramid/history.
    RasterIndirectCommand cmd = pcH.commands.Get().rows[i];
    if (cmd.indexCount == 0u || cmd.instanceCount == 0u ||
        cmd.firstInstance >= GS_RASTER_MAX_WORK ||
        pcH.mips == 0u || pcH.width == 0u || pcH.height == 0u)
        return;

    const RasterWorkRow work = pcH.work.Get().rows[cmd.firstInstance];
    // Masked cards contain cutout holes. Until per-texel coverage-aware
    // cluster occlusion lands, never reject the masked draw from Hi-Z;
    // this fails open without changing opaque occlusion behavior.
    if ((work.detail.z & 2u) != 0u)
        return;
    if (work.identity.x >= min(pcH.instanceSlots, GS_RASTER_MAX_INSTANCES) ||
        work.identity.y == 0u)
        return; // VS already guards stale work; never occlusion-kill it here.
    const GpuSceneInstanceRow row = pcH.instances.Get().rows[work.identity.x];
    if (row.identity.y != 1u || row.identity.x != work.identity.y)
        return;
    // A previous-visible seed is drawn INTO this pyramid. Never test
    // such a command against its own depth; only the remainder can be
    // occluded by seed geometry. Invalid history fails open.
    if (pcH.historyValid != 0u &&
        work.identity.x < min(pcH.historySlots, GS_RASTER_MAX_INSTANCES) &&
        pcH.previous.Get().generations[work.identity.x] == work.identity.y)
        return;
    const GpuSceneInstance inst = row.value;
    // A single-LOD mesh can be raster-admitted without proven bounds
    // (the existing frustum culler fails open). NEVER treat its default
    // localSphere as an occlusion bound. Validate the mesh generation,
    // plus the all-LOD conservative bounds flag before testing.
    if (inst.refs.x >= min(pcH.meshSlots, GS_RASTER_MAX_INSTANCES))
        return;
    const GpuSceneMeshRow mesh = pcH.meshes.Get().rows[inst.refs.x];
    if (mesh.identity.y != 1u || mesh.identity.x != inst.refs.y ||
        (mesh.value.flags & 1u) == 0u)
        return;

    // Both the GPU-scene view and the model are CAMERA-RELATIVE. The
    // double-float subtraction must precede all float projection math.
    const float3 originRelative =
        DoubleFloatRelative(inst.worldPosition, pcH.view.Get().origin);
    const float3 x = inst.basisX.xyz;
    const float3 y = inst.basisY.xyz;
    const float3 z = inst.basisZ.xyz;
    const float4x4 model = float4x4(
        x.x, y.x, z.x, originRelative.x,
        x.y, y.y, z.y, originRelative.y,
        x.z, y.z, z.z, originRelative.z,
        0.0f, 0.0f, 0.0f, 1.0f);

    GfxHzbCtx ctx;
    ctx.viewProj = pcH.view.Get().relativeViewProjection;
    ctx.eye = float3(0.0f, 0.0f, 0.0f);
    ctx.width = pcH.width;
    ctx.height = pcH.height;
    ctx.mips = pcH.mips;
    ctx.hzb = pcH.hiz;

    // Meshes with incomplete bounds remain visible. For valid bounds,
    // the test is conservative: a projected union sphere is rejected only
    // if its nearest possible depth is behind EVERY covered Hi-Z texel.
    // Submesh fallback commands use the conservative all-LOD sphere.
    // Primary meshlet commands use their own local cluster sphere instead.
    // Invalid registration/candidate metadata cannot prove occlusion.
    float4 localSphere = inst.localSphere;
    if (pcH.meshletActive != 0u) {
        if (pcH.meshletLanes == 0u ||
            pcH.commandCapacity % 4u != 0u ||
            pcH.commandCapacity / 4u < pcH.meshletLanes)
            return;
        const uint binStride = pcH.commandCapacity / 4u;
        const uint lane = (i % binStride) % pcH.meshletLanes;
        const uint transientId = work.identity.z;
        if (transientId >= min(pcH.gfxMeshCount, 65535u))
            return;
        const GfxMeshEntry mesh = pcH.gfxMeshes.Get().rows[transientId];
        if (mesh.clusterCount != 0u &&
            mesh.clusterCount <= pcH.meshletLanes) {
            if (lane >= mesh.clusterCount ||
                mesh.clusterStart >= min(pcH.gfxClusterCount, GS_RASTER_MAX_INSTANCES) ||
                lane >= min(pcH.gfxClusterCount, GS_RASTER_MAX_INSTANCES) -
                        mesh.clusterStart)
                return;
            localSphere = pcH.gfxClusters.Get().rows[mesh.clusterStart + lane].b;
            if (!all(isfinite(localSphere)) || localSphere.w <= 0.0f)
                return; // missing cluster bounds keep
        }
    }
    const BoundProjection p = ProjectBound(localSphere, model, ctx);
    if (p.testable && p.onTarget && BoundHzbOccluded(p, ctx)) {
        cmd.instanceCount = 0u;
        pcH.commands.Get().rows[i] = cmd;
        // The final depth remainder must match Main's visibility
        // decision. Seed commands were omitted from this buffer already.
        RasterIndirectCommand rem = pcH.remainder.Get().rows[i];
        rem.instanceCount = 0u;
        pcH.remainder.Get().rows[i] = rem;
    }
}
