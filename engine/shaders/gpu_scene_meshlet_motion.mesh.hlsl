// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Batched motion-vector mesh shader for the GPU Scene task/mesh path.
// One mesh workgroup per surviving cluster (the same task shader as the
// lit meshlet path dispatches it). Emits the current clip position
// (SV_Position) and the previous-frame clip position (prevClip), plus the
// per-instance material id and UV for the MASK alpha test in
// gpu_scene_motion.frag.hlsl. The previous transform is generation-validated
// against the current instance so a recycled slot never ghosts motion.
#define GFXMESH_PASS
#include "common.hlsli"
#include "gpu_scene_task_mesh.hlsli"

GPU_PUSH(GsTaskMeshPush, pc);

struct GsMotionVertex {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 prevClip : PREVCLIP;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] nointerpolation uint materialId : TEXCOORD1;
};

struct GsMotionPrimitive {
    bool cull : SV_CullPrimitive;
};

// Per-instance current + previous transform frame, hoisted out of the
// per-vertex loop. basis columns + origin-relative translation for both
// frames; hasPrev = the previous transform is generation-validated.
struct GsMotionFrame {
    float3 a, b, c;              // current basis columns
    float3 originRelative;       // current DoubleFloatRelative(worldPos, view.origin)
    float4x4 viewProj;           // current relativeViewProjection
    float3 pa, pb, pc;           // previous basis columns
    float3 prevOriginRelative;   // previous origin-relative translation
    float4x4 prevViewProj;       // previous relativeViewProjection
    bool hasPrev;                // previous transform is generation-validated
};

[outputtopology("triangle")]
[numthreads(32,1,1)]
void mainMS(uint3 gid : SV_GroupID,
            uint3 tid : SV_GroupThreadID,
            in payload GsTaskPayload task,
            out vertices GsMotionVertex verts[GS_MESH_MAX_VERTS],
            out indices uint3 tris[GS_MESH_MAX_TRIS],
            out primitives GsMotionPrimitive prims[GS_MESH_MAX_TRIS]) {
    const GsTaskMeshParams p = pc.params.Get();
    bool valid = gid.x < task.surviving && gid.x < GS_TASK_THREADS &&
                 task.workSlot < p.workCount && task.workSlot < GS_MAX_WORK;
    GsWorkRow work = (GsWorkRow)0;
    GpuSceneInstance inst = (GpuSceneInstance)0;
    GsCluster c = (GsCluster)0;
    if (valid) {
        work = p.work.Get().rows[task.workSlot];
        valid = work.identity.y != 0u && work.identity.x < p.instanceCount &&
                work.identity.x < GS_MAX_INSTANCES &&
                work.detail.z == pc.rasterClass;
    }
    if (valid) {
        const GpuSceneInstanceRow row = p.instances.Get().rows[work.identity.x];
        valid = row.identity.y == 1u && row.identity.x == work.identity.y;
        if (valid) inst = row.value;
    }
    if (valid) {
        const uint clusterId = task.clusterIds[gid.x];
        valid = clusterId < p.clusterCount && clusterId < GS_MAX_CLUSTERS;
        if (valid) c = p.clusters.Get().rows[clusterId];
    }
    uint vertexCount = valid ? c.a.w : 0u;
    uint triCount = valid ? c.a.y : 0u;
    if (vertexCount > GS_MESH_MAX_VERTS || triCount > GS_MESH_MAX_TRIS ||
        c.a.z > p.vertexCapacity || vertexCount > p.vertexCapacity - c.a.z ||
        c.e.x > p.indexWordCapacity / 3u || triCount > p.indexWordCapacity / 3u - c.e.x) {
        vertexCount = 0u;
        triCount = 0u;
    }
    SetMeshOutputCounts(vertexCount, triCount);
    if (vertexCount == 0u || triCount == 0u) return;

    const GsRasterView view = p.rasterView.Get();
    const uint slot = work.identity.x;
    const uint generation = work.identity.y;

    GsMotionFrame f = (GsMotionFrame)0;
    f.a = inst.basisX.xyz;
    f.b = inst.basisY.xyz;
    f.c = inst.basisZ.xyz;
    f.originRelative = DoubleFloatRelative(inst.worldPosition, view.origin);
    f.viewProj = view.relativeViewProjection;
    // Previous transform, generation-validated. A recycled slot (generation
    // changed since the snapshot) keeps zero motion rather than ghosting the
    // previous owner's transform. slot is also bounded by prevSlots.
    f.pa = f.a; f.pb = f.b; f.pc = f.c;
    f.prevOriginRelative = f.originRelative;
    f.prevViewProj = view.relativeViewProjection;
    f.hasPrev = false;
    if (p.prevValid != 0u && slot < p.prevSlots) {
        const GsPrevTransform prev = p.prevTransforms.Get().rows[slot];
        if (prev.generation.x == generation) {
            f.pa = prev.basisX.xyz;
            f.pb = prev.basisY.xyz;
            f.pc = prev.basisZ.xyz;
            f.prevOriginRelative =
                DoubleFloatRelative(prev.worldPosition, p.prevView.Get().origin);
            f.prevViewProj = p.prevView.Get().relativeViewProjection;
            f.hasPrev = true;
        }
    }

    const bool skinned = (inst.refs.w & GPU_SCENE_INSTANCE_SKINNED) != 0u;
    const uint materialId = inst.refs.z;
    const uint vertexBase = c.a.z;
    const uint triBase = c.e.x;

    for (uint v = tid.x; v < vertexCount; v += GS_MESH_THREADS) {
        const uint globalId = p.meshletVertices.Get().id[vertexBase + v];
        const uint pb = globalId * 3u, sb = globalId * 4u;
        float3 local = float3(
            asfloat(p.positions.Get().word[pb]),
            asfloat(p.positions.Get().word[pb + 1u]),
            asfloat(p.positions.Get().word[pb + 2u]));
        // Skinned fetch: swap the bind-pose position for the frame's DEFORMED
        // words. A null BDA / unknown base means the frame ran no skinning pass
        // and the bind pose is the honest answer.
        if (skinned && pc.skinValid != 0u) {
            const uint d = (globalId - pc.skinDeformedBase) * 5u;
            local = float3(
                asfloat(pc.skinDeformed.Get().word[d + 0u]),
                asfloat(pc.skinDeformed.Get().word[d + 1u]),
                asfloat(pc.skinDeformed.Get().word[d + 2u]));
        }
        // Previous deformed position (skeletal motion): the previous parity
        // half of the SAME heap/base when valid. Otherwise fall back to `local`
        // (the current deformed pose, or bind pose when the frame skinned
        // nothing) — this preserves rigid + camera motion while suppressing
        // unavailable skeletal motion instead of ghosting it.
        float3 prevLocal = local;
        if (skinned && pc.skinPrevValid != 0u) {
            const uint pd = (globalId - pc.skinDeformedBase) * 5u;
            prevLocal = float3(
                asfloat(pc.skinDeformedPrev.Get().word[pd + 0u]),
                asfloat(pc.skinDeformedPrev.Get().word[pd + 1u]),
                asfloat(pc.skinDeformedPrev.Get().word[pd + 2u]));
        }
        const uint uvword = p.shading.Get().word[sb + 1u];
        const float2 uv = float2(f16tof32(uvword & 65535u), f16tof32(uvword >> 16u));

        const float3 relative =
            f.originRelative + f.a * local.x + f.b * local.y + f.c * local.z;
        verts[v].pos = mul(f.viewProj, float4(relative, 1.0f));
        verts[v].uv = uv;
        verts[v].materialId = materialId;
        if (f.hasPrev) {
            const float3 prevRelative = f.prevOriginRelative +
                f.pa * prevLocal.x + f.pb * prevLocal.y + f.pc * prevLocal.z;
            verts[v].prevClip = mul(f.prevViewProj, float4(prevRelative, 1.0f));
        } else {
            // First frame / camera cut: zero motion.
            verts[v].prevClip = verts[v].pos;
        }
    }

    for (uint t = tid.x; t < triCount; t += GS_MESH_THREADS) {
        const uint packed = p.meshletTriangles.Get().tri[triBase + t];
        tris[t] = uint3(packed & 255u, (packed >> 8u) & 255u, (packed >> 16u) & 255u);
        prims[t].cull = false;
    }
}
