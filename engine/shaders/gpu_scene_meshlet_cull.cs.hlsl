// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Per-meshlet GPU-scene indexed command gate (staged, not yet dispatched).
// Each candidate owns one command slot. Non-testable bounds KEEP geometry;
// stale instance/work identities emit an empty command. Never compact work
// slots here: SV_InstanceID must remain the original GPU-scene work index.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GS_MESHLET_MAX_CANDIDATES 4194304u
#define GS_MESHLET_MAX_WORK 1048576u
#define GS_MESHLET_MAX_INSTANCES 4194304u

struct MeshletCandidate {
    uint firstIndex;
    uint indexCount;
    uint workSlot;
    uint flags;
    float4 sphere;
};
struct MeshletCandidates { MeshletCandidate rows[GS_MESHLET_MAX_CANDIDATES]; };
struct RasterWorkRow { uint4 identity; uint4 detail; };
struct RasterWork { RasterWorkRow rows[GS_MESHLET_MAX_WORK]; };
struct RasterInstances { GpuSceneInstanceRow rows[GS_MESHLET_MAX_INSTANCES]; };
struct IndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct IndirectCommands { IndirectCommand rows[GS_MESHLET_MAX_CANDIDATES]; };
struct MeshletCullView {
    DoubleFloat3 origin;
    float4 inwardPlanes[6];
};
struct MeshletCullPush {
    GPU_PTR_READ(MeshletCandidates) candidates;
    GPU_PTR_READ(RasterWork) work;
    GPU_PTR_READ(RasterInstances) instances;
    GPU_PTR_READ(MeshletCullView) view;
    GPU_PTR_WRITE(IndirectCommands) commands;
    uint candidateCount;
    uint workCount;
    uint instanceCount;
    uint viewValid;
};
GPU_PUSH(MeshletCullPush, pcM);

bool MeshletSphereVisible(float4 sphere, GpuSceneInstance inst, MeshletCullView view) {
    // Unknown/degenerate bounds cannot prove a cluster invisible.
    if (!all(isfinite(sphere)) || sphere.w <= 0.0f)
        return true;
    const float3 bx = inst.basisX.xyz;
    const float3 by = inst.basisY.xyz;
    const float3 bz = inst.basisZ.xyz;
    const float3 center = DoubleFloatRelative(inst.worldPosition, view.origin)
        + bx * sphere.x + by * sphere.y + bz * sphere.z;
    const float radius = sphere.w * sqrt(max(
        dot(bx, bx) + dot(by, by) + dot(bz, bz), 0.0f));
    if (!all(isfinite(center)) || !isfinite(radius))
        return true;
    [unroll]
    for (uint i = 0u; i < 6u; ++i) {
        const float4 plane = view.inwardPlanes[i];
        if (!all(isfinite(plane))) return true;
        const float d = dot(plane.xyz, center) + plane.w;
        if (!isfinite(d)) return true;
        const float slack = 0.02f + 2e-6f * (abs(d) + radius + 1.0f);
        if (d < -(radius + slack)) return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pcM.candidateCount || i >= GS_MESHLET_MAX_CANDIDATES)
        return;
    IndirectCommand cmd = (IndirectCommand)0;
    pcM.commands.Get().rows[i] = cmd;
    const MeshletCandidate candidate = pcM.candidates.Get().rows[i];
    if (candidate.indexCount == 0u ||
        candidate.workSlot >= min(pcM.workCount, GS_MESHLET_MAX_WORK))
        return;
    const RasterWorkRow work = pcM.work.Get().rows[candidate.workSlot];
    if (work.identity.y == 0u ||
        work.identity.x >= min(pcM.instanceCount, GS_MESHLET_MAX_INSTANCES))
        return;
    const GpuSceneInstanceRow row = pcM.instances.Get().rows[work.identity.x];
    if (row.identity.y != 1u || row.identity.x != work.identity.y)
        return;
    if (pcM.viewValid != 0u &&
        !MeshletSphereVisible(candidate.sphere, row.value, pcM.view.Get()))
        return;
    cmd.indexCount = candidate.indexCount;
    cmd.instanceCount = 1u;
    cmd.firstIndex = candidate.firstIndex;
    cmd.vertexOffset = 0;
    cmd.firstInstance = candidate.workSlot;
    pcM.commands.Get().rows[i] = cmd;
}
