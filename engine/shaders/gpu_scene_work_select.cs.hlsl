// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU Scene: select the finest registered/resident LOD for each compacted
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

    // LOD0 is finest. Until the shared screen-error selector is moved
    // into this stage, use the first fully registered resident LOD;
    // never guess a deferred LOD's geometry.
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
        choice.identity = uint4(verdict.x, verdict.y, lodIndex,
                                 lod.value.firstSubmesh);
        choice.range = uint4(lod.value.submeshCount, workCount, 0u, 0u);
        pcW.choices.Get().rows[i] = choice;
        pcW.counts.Get().values[i] = workCount;
        return;
    }
}
