// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU Scene: select the coarsest in-budget registered/resident LOD for each
// visible instance and count its valid submeshes. No draw side effects.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GS_MAX_CANDIDATES 1048576u
#define GS_MAX_GROUPS 16384u
#define GS_MAX_ROWS 4194304u
#define GS_MAX_LODS 16u
#define GS_MAX_SUBMESHES 16u
#define GS_INVALID 0xffffffffu
struct Verdict { uint4 value; };
struct VerdictBuffer { Verdict rows[GS_MAX_CANDIDATES]; };
struct OffsetBuffer { uint values[GS_MAX_GROUPS + 1u]; };
struct Instances { GpuSceneInstanceRow rows[GS_MAX_ROWS]; };
struct Meshes { GpuSceneMeshRow rows[GS_MAX_ROWS]; };
struct Lods { GpuSceneLodRow rows[GS_MAX_ROWS]; };
struct Submeshes { GpuSceneSubmeshRow rows[GS_MAX_ROWS]; };
struct WorkChoice { uint4 identity; uint4 range; };
// identity: instance slot, instance generation, LOD index, first submesh.
// range: original submesh count, valid work count, reserved, reserved.
struct Choices { WorkChoice rows[GS_MAX_CANDIDATES]; };
struct Counts { uint values[GS_MAX_CANDIDATES]; };
struct Push {
    GPU_PTR_READ(VerdictBuffer) compact;
    GPU_PTR_READ(OffsetBuffer) compactOffsets;
    GPU_PTR_READ(Instances) instances;
    GPU_PTR_READ(Meshes) meshes;
    GPU_PTR_READ(Lods) lods;
    GPU_PTR_READ(Submeshes) submeshes;
    GPU_PTR_WRITE(Choices) choices;
    GPU_PTR_WRITE(Counts) counts;
    uint4 limits; // candidates, groups, instance slots, mesh slots
    uint4 limits2; // lod slots, submesh slots, reserved, reserved
    float4 lodEyeBudget; // absolute float camera xyz, pixel budget w
    float4 lodProjection; // viewport height, vertical FOV radians, enabled, reserved
};
GPU_PUSH(Push, pcW);

[numthreads(64,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pcW.limits.x || i >= GS_MAX_CANDIDATES) return;
    WorkChoice choice;
    choice.identity = uint4(GS_INVALID, 0u, GS_INVALID, GS_INVALID);
    choice.range = uint4(0u,0u,0u,0u);
    pcW.choices.Get().rows[i] = choice;
    pcW.counts.Get().values[i] = 0u;
    if (pcW.limits.y > GS_MAX_GROUPS) return;
    const uint visible = pcW.compactOffsets.Get().values[pcW.limits.y];
    if (i >= visible) return;
    const uint4 verdict = pcW.compact.Get().rows[i].value;
    if (verdict.x == GS_INVALID || verdict.y == 0u || verdict.z == 0u ||
        verdict.x >= min(pcW.limits.z, GS_MAX_ROWS)) return;
    const GpuSceneInstanceRow inst = pcW.instances.Get().rows[verdict.x];
    if (inst.identity.y != 1u || inst.identity.x != verdict.y) return;
    const uint meshIndex = inst.value.refs.x;
    if (meshIndex >= min(pcW.limits.w, GS_MAX_ROWS)) return;
    const GpuSceneMeshRow mesh = pcW.meshes.Get().rows[meshIndex];
    if (mesh.identity.y != 1u || mesh.identity.x != inst.value.refs.y ||
        mesh.value.lodCount == 0u || mesh.value.lodCount > GS_MAX_LODS)
        return;
    const uint lodSlots = min(pcW.limits2.x, GS_MAX_ROWS);
    const uint subSlots = min(pcW.limits2.y, GS_MAX_ROWS);
    if (mesh.value.firstLod >= lodSlots ||
        mesh.value.lodCount > lodSlots - mesh.value.firstLod) return;

    // Match the World's sphere-surface distance and per-instance max axis
    // scale (mesh simplification errors are authored in local units).
    // Near/inside bounds, invalid view data, or disabled LOD use the finest
    // available LOD. This first GPU selection has no previous-LOD hysteresis.
    const float3 bx = inst.value.basisX.xyz;
    const float3 by = inst.value.basisY.xyz;
    const float3 bz = inst.value.basisZ.xyz;
    const float scale = max(length(bx), max(length(by), length(bz)));
    const float3 center =
        (inst.value.worldPosition.high.xyz - pcW.lodEyeBudget.xyz) +
        inst.value.worldPosition.low.xyz +
        bx * inst.value.localSphere.x + by * inst.value.localSphere.y +
        bz * inst.value.localSphere.z;
    const float radius = inst.value.localSphere.w *
                         sqrt(max(dot(bx,bx) + dot(by,by) + dot(bz,bz), 0.0f));
    const float distance = max(length(center) - radius, 0.0f);
    const bool useError = pcW.lodProjection.z != 0.0f &&
        pcW.lodEyeBudget.w >= 0.0f && isfinite(pcW.lodEyeBudget.w) &&
        pcW.lodProjection.x > 0.0f &&
        pcW.lodProjection.y > 0.0f && pcW.lodProjection.y < 3.14159f &&
        distance > 0.0f && isfinite(distance) &&
        scale > 0.0f && isfinite(scale);
    const float pixelsPerWorldUnit = useError ?
        pcW.lodProjection.x / (2.0f * tan(pcW.lodProjection.y * 0.5f) * distance) : 0.0f;
    WorkChoice selected = choice;
    uint selectedWork = 0u;
    bool found = false;
    // LOD0 is finest. Scan all valid resident levels; choose the coarsest
    // within the pixel budget, while retaining the finest valid fallback.
    for (uint lodIndex = 0u; lodIndex < mesh.value.lodCount; ++lodIndex) {
        if ((mesh.value.residentMask & (1u << lodIndex)) == 0u) continue;
        const GpuSceneLodRow lod =
            pcW.lods.Get().rows[mesh.value.firstLod + lodIndex];
        if (lod.identity.y != 1u || (lod.value.flags & 5u) != 5u ||
            lod.value.submeshCount == 0u ||
            lod.value.submeshCount > GS_MAX_SUBMESHES ||
            lod.value.firstSubmesh >= subSlots ||
            lod.value.submeshCount > subSlots - lod.value.firstSubmesh)
            continue;
        uint workCount = 0u;
        for (uint j = 0u; j < lod.value.submeshCount; ++j) {
            const GpuSceneSubmeshRow sub =
                pcW.submeshes.Get().rows[lod.value.firstSubmesh + j];
            if (sub.identity.y == 1u && sub.value.geometryId != GS_INVALID)
                ++workCount;
        }
        if (workCount == 0u) continue;
        if (found && (!useError || !isfinite(pixelsPerWorldUnit) ||
            !isfinite(lod.value.simplifyError) || lod.value.simplifyError < 0.0f ||
            lod.value.simplifyError * scale * pixelsPerWorldUnit > pcW.lodEyeBudget.w))
            continue;
        selected.identity = uint4(verdict.x, verdict.y, lodIndex,
                                  lod.value.firstSubmesh);
        selected.range = uint4(lod.value.submeshCount, workCount,
                               ((inst.value.refs.w & GPU_SCENE_INSTANCE_DOUBLE_SIDED) != 0u
                                    ? 1u : 0u) |
                               ((inst.value.refs.w & GPU_SCENE_INSTANCE_MASKED) != 0u
                                    ? 2u : 0u), 0u);
        selectedWork = workCount;
        found = true;
    }
    if (found) {
        pcW.choices.Get().rows[i] = selected;
        pcW.counts.Get().values[i] = selectedWork;
    }
}
