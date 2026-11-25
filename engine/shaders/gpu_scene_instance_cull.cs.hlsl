// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPUIR GPU instance frustum test — opt-in, not dispatched by WorldRenderer.
// CPU produces a small, sorted sequence of PassedInstanceRange records
// after COARSE leaf selection. One thread owns one candidate output slot;
// it binary-searches the range prefix, validates generational references,
// and writes a DETERMINISTIC verdict. No unsafe atomic compaction here.
//
// All view planes are render-origin-relative. Absolute scene translations
// are stored as DoubleFloat3 and rebased BEFORE any float32 cull math.
// This compute stage feeds future visibility/LOD expansion, not GfxDraw.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"
#include "hzb.hlsli"

#define GPU_SCENE_MAX_PASSED_RANGES   65536u
#define GPU_SCENE_MAX_CANDIDATES      1048576u
#define GPU_SCENE_MAX_ROW_SLOTS       4194304u

struct GpuScenePassedRange {
    uint4 members; // x member row first, y count, z candidate prefix
    uint4 leaf;    // x leaf slot, y leaf generation
};
struct GpuSceneCullVerdict {
    uint4 value;   // x instance slot, y generation, z visible, w flags
};
struct GpuScenePassedRanges {
    GpuScenePassedRange ranges[GPU_SCENE_MAX_PASSED_RANGES];
};
struct GpuSceneCullVerdicts {
    GpuSceneCullVerdict values[GPU_SCENE_MAX_CANDIDATES];
};
struct GpuSceneOwnerGenerations {
    uint generations[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneLeaves {
    GpuSceneLeafRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneMembers {
    GpuSceneLeafMemberRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneInstances {
    GpuSceneInstanceRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneMeshes {
    GpuSceneMeshRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
// Explicit per-view data buffer instead of 128+ bytes of push constants.
struct GpuSceneCullView {
    DoubleFloat3 origin;
    float4 inwardPlanes[6]; // normal.xyz, d; relative to origin
};
// Shadow-caster HZB occlusion context (C++ mirror: GpuSceneShadowHizView).
struct GpuSceneShadowHizView {
    float4x4 mainViewProj;   // main view, camera-relative VP (Y-flipped)
    float4 sunDirAndSweep;   // xyz = light dir (unit, sun->scene), w = sweep cap (m)
    float4 depthRange;       // x = near clip (m), y = furthest visible depth (m)
    uint4 hzbDesc;           // x width, y height, z mips, w valid (0 = off)
    GPU_PTR_READ(HzbBuffer) hzb;
    uint2 pad;
};
struct GpuSceneInstanceCullPush {
    GPU_PTR_READ(GpuScenePassedRanges) ranges;
    GPU_PTR_READ(GpuSceneLeaves) leaves;
    GPU_PTR_READ(GpuSceneMembers) members;
    GPU_PTR_READ(GpuSceneInstances) instances;
    GPU_PTR_READ(GpuSceneMeshes) meshes;
    GPU_PTR_READ(GpuSceneCullView) view;
    GPU_PTR_WRITE(GpuSceneCullVerdicts) verdicts;
    uint4 counts; // x rangeCount, y candidateCount, z memberSlots, w instanceSlots
    uint4 counts2; // x leafSlots, y meshSlots, z viewValid (0=passthrough), w owner-mask enabled
    GPU_PTR_READ(GpuSceneOwnerGenerations) ownerGenerations; // null in CPU oracle mode
    // Shadow-caster early-out: xyz = main camera forward (unit), w = the
    // furthest visible view depth (m), or 0 to disable. The CPU only sets
    // w > 0 when the sun travels away from the camera (shadow deepens), so
    // a caster deeper than w can never shadow a visible surface.
    float4 shadowCullParams;
    GPU_PTR_READ(GpuSceneShadowHizView) shadowHiz; // null = HZB cull off
};
GPU_PUSH(GpuSceneInstanceCullPush, pcS);

bool GpuSceneSphereVisible(GpuSceneInstance instance, GpuSceneCullView view) {
    const float3 translation = DoubleFloatRelative(
        instance.worldPosition, view.origin);
    const float3 bx = instance.basisX.xyz;
    const float3 by = instance.basisY.xyz;
    const float3 bz = instance.basisZ.xyz;
    const float3 local = instance.localSphere.xyz;
    const float3 center = translation + bx * local.x
        + by * local.y + bz * local.z;
    const float basisSquared = dot(bx, bx) + dot(by, by) + dot(bz, bz);
    const float radius = sqrt(max(basisSquared, 0.0f))
                       * instance.localSphere.w;
    // Non-finite or unknown bounds must never prove invisibility.
    if (!all(isfinite(center)) || !isfinite(radius) || radius < 0.0f)
        return true;
    [unroll]
    for (uint plane = 0; plane < 6u; ++plane) {
        float4 p = view.inwardPlanes[plane];
        const float d = dot(p.xyz, center) + p.w;
        if (!isfinite(d) || !all(isfinite(p)))
            return true;
        // Conservative numeric slack. Deliberately larger for faraway
        // candidates: large relative float32 positions lose precision.
        const float slack = 0.02f + 2e-6f * (abs(d) + radius + 1.0f);
        if (d < -(radius + slack))
            return false;
    }
    return true;
}

// Swept-shadow-volume occlusion: extrude a caster sphere (center, radius,
// camera-relative) along the sun direction up to the visible-depth bound,
// project the resulting swept AABB into the MAIN view, and test it against
// the reversed-Z MIN pyramid (near = 1, far = 0; a texel stores the FARTHEST
// depth in its region). Returns true only when the ENTIRE swept volume is
// provably behind the visible depth — a caster whose shadow can only land on
// occluded surface and is therefore safe to drop. Every failure path keeps
// (returns false); unknown bounds never cull.
bool SweptShadowOccluded(float3 center, float radius, float3 camFwd,
                         GpuSceneShadowHizView hz)
{
    if (hz.hzbDesc.w == 0u)
        return false;
    const float width = float(hz.hzbDesc.x);
    const float height = float(hz.hzbDesc.y);
    if (!(width > 0.0f) || !(height > 0.0f) || hz.hzbDesc.z == 0u)
        return false;

    // Bound the sweep by the visible depth range: the shadow (extruded along
    // the sun) is only visible while it stays within [near, Zmax] view depth.
    const float3 lightDir = hz.sunDirAndSweep.xyz;
    const float sweepCap = max(hz.sunDirAndSweep.w, 0.0f);
    const float nearClip = hz.depthRange.x;
    const float zMax = hz.depthRange.y;
    const float LdotV = dot(lightDir, camFwd);
    const float dC = dot(center, camFwd);
    float sMax = sweepCap;
    if (LdotV > 1e-4f)
        sMax = clamp((zMax - dC + radius) / LdotV, 0.0f, sweepCap);
    else if (LdotV < -1e-4f)
        sMax = clamp((dC - radius - nearClip) / (-LdotV), 0.0f, sweepCap);

    // Swept AABB of the sphere extruded along +lightDir.
    const float3 sweep = lightDir * sMax;
    const float3 aabbMin = center - radius + min(sweep, float3(0.0f, 0.0f, 0.0f));
    const float3 aabbMax = center + radius + max(sweep, float3(0.0f, 0.0f, 0.0f));

    // Project the 8 AABB corners; track the screen rect + the NEAREST
    // reversed-Z depth (largest z/w). A corner behind the camera plane
    // fails open (keep).
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    float dNear = -1e30f;
    [unroll]
    for (uint c = 0; c < 8u; ++c) {
        const float3 p = float3(
            (c & 1u) != 0u ? aabbMax.x : aabbMin.x,
            (c & 2u) != 0u ? aabbMax.y : aabbMin.y,
            (c & 4u) != 0u ? aabbMax.z : aabbMin.z);
        const float4 clip = mul(hz.mainViewProj, float4(p, 1.0f));
        if (!(clip.w > 1e-6f))
            return false; // swept volume crosses / sits behind the camera
        const float3 ndc = clip.xyz / clip.w;
        minX = min(minX, ndc.x);
        maxX = max(maxX, ndc.x);
        minY = min(minY, ndc.y);
        maxY = max(maxY, ndc.y);
        dNear = max(dNear, ndc.z);
    }
    if (!(dNear >= 0.0f) || dNear > 1.0f)
        return false;

    const float x0 = (minX * 0.5f + 0.5f) * width;
    const float x1 = (maxX * 0.5f + 0.5f) * width;
    const float y0 = (minY * 0.5f + 0.5f) * height;
    const float y1 = (maxY * 0.5f + 0.5f) * height;
    // Fully off-target: the shadow cannot be visible; the frustum stages own
    // that decision, so keep (this test only proves occlusion).
    if (x1 < 0.0f || x0 > width || y1 < 0.0f || y0 > height)
        return false;
    if (!(x1 >= x0) || !(y1 >= y0))
        return false;

    // Choose the first mip whose nominal source footprint covers the longest
    // axis, then walk the covered texels (matches BoundHzbOccluded).
    const float rectW = x1 - x0;
    const float rectH = y1 - y0;
    if (!(rectW > 0.0f) || !(rectH > 0.0f))
        return false;
    uint level = 0u;
    float texel = 1.0f;
    const float rectMax = max(rectW, rectH);
    while (texel < rectMax && level + 1u < hz.hzbDesc.z) {
        texel *= 2.0f;
        ++level;
    }
    uint lw = hz.hzbDesc.x;
    uint lh = hz.hzbDesc.y;
    uint off = 0u;
    for (uint k = 0u; k < level; ++k) {
        off += lw * lh;
        lw = max((lw + 1u) >> 1, 1u);
        lh = max((lh + 1u) >> 1, 1u);
    }
    if (lw == 0u || lh == 0u)
        return false;

    const uint tx0 = (uint) clamp(floor(x0 / texel), 0.0f, float(lw - 1u));
    const uint tx1 = (uint) clamp(floor(x1 / texel), 0.0f, float(lw - 1u));
    const uint ty0 = (uint) clamp(floor(y0 / texel), 0.0f, float(lh - 1u));
    const uint ty1 = (uint) clamp(floor(y1 / texel), 0.0f, float(lh - 1u));
    if (tx0 > tx1 || ty0 > ty1)
        return false;

    // Conservative reversed-Z bias toward VISIBLE (matches BoundHzbOccluded).
    const float depthBias = max(1e-7f, abs(dNear) * 1e-5f);
    const float testDepth = dNear + depthBias;

    // MIN HZB under reversed-Z: stored = farthest depth in the coarse texel.
    // Occluded only when the swept volume's nearest depth is behind EVERY
    // covered texel (testDepth < stored for all).
    for (uint ty = ty0; ty <= ty1; ++ty) {
        for (uint tx = tx0; tx <= tx1; ++tx) {
            const float stored = hz.hzb.Get().texels[off + ty * lw + tx];
            if (testDepth >= stored)
                return false;
        }
    }
    return true;
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint candidate = tid.x;
    if (candidate >= pcS.counts.y || candidate >= GPU_SCENE_MAX_CANDIDATES)
        return;

    // Always initialize; no previous-frame verdict can leak into this slot.
    GpuSceneCullVerdict result;
    result.value = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
    pcS.verdicts.Get().values[candidate] = result;

    const uint rangeCount = min(pcS.counts.x, GPU_SCENE_MAX_PASSED_RANGES);
    if (rangeCount == 0u)
        return;

    // Upper_bound(range.members.z, candidate). Candidate prefix offsets
    // are monotonically increasing, generated and capped by the CPU.
    uint lo = 0u, hi = rangeCount;
    while (lo < hi) {
        const uint mid = lo + (hi - lo) / 2u;
        const uint prefix = pcS.ranges.Get().ranges[mid].members.z;
        if (prefix <= candidate) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u)
        return;
    const GpuScenePassedRange range =
        pcS.ranges.Get().ranges[lo - 1u];
    const uint local = candidate - range.members.z;
    if (local >= range.members.y)
        return;

    // Validate the entire leaf-to-member relationship against this flight
    // snapshot, including its generation and current contiguous span.
    if (range.leaf.x >= min(pcS.counts2.x, GPU_SCENE_MAX_ROW_SLOTS))
        return;
    const GpuSceneLeafRow leaf = pcS.leaves.Get().rows[range.leaf.x];
    if (leaf.identity.y != 1u || leaf.identity.x != range.leaf.y ||
        leaf.value.range.x != range.members.x ||
        leaf.value.range.y != range.members.y)
        return;
    if (range.members.x >= min(pcS.counts.z, GPU_SCENE_MAX_ROW_SLOTS) ||
        local >= min(pcS.counts.z, GPU_SCENE_MAX_ROW_SLOTS) - range.members.x)
        return;

    const uint memberIndex = range.members.x + local;
    const GpuSceneLeafMemberRow member =
        pcS.members.Get().rows[memberIndex];
    if (member.identity.y != 1u)
        return;
    const uint instanceId = member.value.ref.x;
    const uint instanceGen = member.value.ref.y;
    if (instanceId >= min(pcS.counts.w, GPU_SCENE_MAX_ROW_SLOTS))
        return;
    const GpuSceneInstanceRow instance =
        pcS.instances.Get().rows[instanceId];
    if (instance.identity.y != 1u ||
        instance.identity.x != instanceGen)
        return;

    // Production GPU Main only: a slot is admitted if and only if this
    // flight's mask records its exact generation. A retired/recycled slot
    // or a legacy-owned (including skinned) source emits no GPU work.
    // Validation mode has a null owner pointer and retains its full oracle.
    // Explicit control flow, not a logical && expression: validation
    // leaves the pointer null and must never issue an owner-mask load.
    if (pcS.counts2.w != 0u) {
        if (pcS.ownerGenerations.Get().generations[instanceId] != instanceGen)
            return;
    }

    // Shadow views must reject non-casters before building work. Bit 1 of
    // counts2.z requests CastShadow; bit 0 retains the view-valid flag.
    if ((pcS.counts2.z & 2u) != 0u &&
        (instance.value.refs.w & (1u << 5)) == 0u)
        return;

    // Unknown/incomplete mesh bounds: fail OPEN, not invisible.
    bool bypass = true;
    if (instance.value.refs.x < min(pcS.counts2.y, GPU_SCENE_MAX_ROW_SLOTS)) {
        const GpuSceneMeshRow mesh =
            pcS.meshes.Get().rows[instance.value.refs.x];
        bypass = mesh.identity.y != 1u ||
                 mesh.identity.x != instance.value.refs.y ||
                 (mesh.value.flags & 1u) == 0u; // MeshFlagCompleteBounds
    }
    // Shadow-caster early-outs (both fail open — never cull unknown bounds):
    //   (1) cheap furthest-visible-depth scalar, and
    //   (2) swept-shadow-volume HZB occlusion.
    bool shadowCulled = false;
    if ((pcS.counts2.z & 2u) != 0u && !bypass) {
        const float3 translation =
            DoubleFloatRelative(instance.value.worldPosition, pcS.view.Get().origin);
        const float3 center = translation +
            instance.value.basisX.xyz * instance.value.localSphere.x +
            instance.value.basisY.xyz * instance.value.localSphere.y +
            instance.value.basisZ.xyz * instance.value.localSphere.z;
        const float basisSquared =
            dot(instance.value.basisX.xyz, instance.value.basisX.xyz) +
            dot(instance.value.basisY.xyz, instance.value.basisY.xyz) +
            dot(instance.value.basisZ.xyz, instance.value.basisZ.xyz);
        const float radius =
            sqrt(max(basisSquared, 0.0f)) * instance.value.localSphere.w;
        if (all(isfinite(center)) && isfinite(radius) && radius >= 0.0f) {
            // (1) Scalar: caster fully beyond the furthest visible depth.
            if (pcS.shadowCullParams.w > 0.0f) {
                const float nearDepth = dot(center, pcS.shadowCullParams.xyz) - radius;
                shadowCulled = isfinite(nearDepth) && nearDepth > pcS.shadowCullParams.w;
            }
            // (2) Swept-volume HZB occlusion (armed when the CPU uploaded a
            // valid context — counts2.z bit 2 — and hzbDesc.w is nonzero).
            if (!shadowCulled && (pcS.counts2.z & 4u) != 0u)
                shadowCulled =
                    SweptShadowOccluded(center, radius, pcS.shadowCullParams.xyz,
                                        pcS.shadowHiz.Get());
        }
    }
    const bool visible = !shadowCulled &&
        (bypass || (pcS.counts2.z & 1u) == 0u ||
         GpuSceneSphereVisible(instance.value, pcS.view.Get()));
    result.value = uint4(instanceId, instanceGen, visible ? 1u : 0u,
                         bypass ? 1u : 0u);
    pcS.verdicts.Get().values[candidate] = result;
}
