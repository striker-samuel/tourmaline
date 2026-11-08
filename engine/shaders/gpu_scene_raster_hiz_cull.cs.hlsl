// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// GPU-scene raster Hi-Z command gate. Runs AFTER the GPU-scene occluder
// depth pass and the reversed-Z MIN pyramid, but BEFORE Main/depth.
// The occluder depth uses the unfiltered frustum-visible commands; the
// final commands can only lose instances, never gain invalid references.
// Unknown, invalid or untestable bounds always KEEP the command.
#include "gpu_scene.hlsli"
#include "gfxfilter.hlsli"

#define GS_RASTER_MAX_WORK 1048576u
#define GS_RASTER_MAX_INSTANCES 4194304u

struct RasterIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct RasterCommands { RasterIndirectCommand rows[GS_RASTER_MAX_WORK]; };
struct RasterWorkRow { uint4 identity; uint4 detail; };
struct RasterWork { RasterWorkRow rows[GS_RASTER_MAX_WORK]; };
struct RasterInstances { GpuSceneInstanceRow rows[GS_RASTER_MAX_INSTANCES]; };
struct RasterHistory { uint generations[GS_RASTER_MAX_INSTANCES]; };
struct RasterViewData {
    float4x4 relativeViewProjection;
    DoubleFloat3 origin;
};
struct RasterHizPush {
    GPU_PTR(RasterCommands) commands;
    GPU_PTR(RasterCommands) remainder;
    GPU_PTR_READ(RasterWork) work;
    GPU_PTR_READ(RasterInstances) instances;
    GPU_PTR_READ(RasterViewData) view;
    GPU_PTR_READ(HzbBuffer) hiz;
    GPU_PTR_READ(RasterHistory) previous;
    uint commandCapacity;
    uint instanceSlots;
    uint width;
    uint height;
    uint mips;
    uint historySlots;
    uint historyValid;
    uint reserved;
};
GPU_PUSH(RasterHizPush, pcH);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pcH.commandCapacity || i >= GS_RASTER_MAX_WORK)
        return;
    RasterIndirectCommand cmd = pcH.commands.Get().rows[i];
    if (cmd.indexCount == 0u || cmd.instanceCount == 0u ||
        cmd.firstInstance >= GS_RASTER_MAX_WORK ||
        pcH.mips == 0u || pcH.width == 0u || pcH.height == 0u)
        return;

    const RasterWorkRow work = pcH.work.Get().rows[cmd.firstInstance];
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
    const BoundProjection p = ProjectBound(inst.localSphere, model, ctx);
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
