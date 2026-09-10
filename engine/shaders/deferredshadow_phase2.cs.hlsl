// Deferred Shadows — Phase 2: Find Lights Approximate (paper listing 2).
//
// One work unit = one TileGroup (8x8 Shadow Tiles = 64x64 px); each thread
// evaluates ONE Shadow Tile (8x8 px) — speed over accuracy, false positives
// filtered by phase 3. Per thread:
//   1. fetch the tile min/max scene depths (8x8 direct depth loads with
//      sky filtering — the paper's HZB footnote: when the far bound hits
//      the far plane/sky, use the farthest VALID surface instead so
//      sky tiles don't pull in the whole depth range; a min/max HZB pair
//      would replace these loads),
//   2. build the Froxel (screen rect x depth range) and look up the
//      zBin(ranged) window [lo..hi] over the sorted light buffer,
//   3. keep lights present in BOTH the CPU Tile Light Mask (TileGroup
//      granularity) and the zBin window,
//   4. vet each candidate with bound intersections vs the froxel:
//      sphere/sphere, then point: sphere-vs-froxel-box / spot:
//      cone/sphere ('Cull That Cone'-style conservative test, derived
//      below) + an axis-aligned box test in the light's clipping space.
//
// Outputs (wave-size-safe: 8x8 threads can be one wave64 or two wave32s,
// so cross-lane reductions go through groupshared, not wave intrinsics):
//   - TileGroupLightMask (WaveActiveBitOr in the paper; InterlockedOr on
//     groupshared here — same result for the whole TileGroup),
//   - Tile Light SlotID per Shadow Tile (listing 1: one atomic for the
//     whole TileGroup + per-tile prefix offset; reservation is in uints
//     of the shared light list sized by the approximated light count —
//     see deferredshadow.hlsli layout notes).
//
// Depth conventions (must match the app): infinite-style reversed-Z,
// clear 0 (far/sky), GreaterEqual; view distance = near / max(d, eps);
// NDC y down in the framebuffer (Y-flip baked into proj).

#define DS_PASS
#include "common.hlsli"

[[vk::push_constant]]
ConstantBuffer<DsPhasePush> pcDS;

groupshared uint gsGrpMasks[DS_MAX_LIGHT_GROUPS]; // per-group OR over 64 tiles
groupshared uint gsCounts[64];                    // reservation per tile (uints)
groupshared uint gsPrefix[64];                    // exclusive prefix within group
groupshared uint gsBase;                          // group's reserved list base

// ---- spot light VP in view space (mirrors smvk::GetSpotLightVP:
// lookAt + perspectiveRH_ZO, aspect 1, no Y-flip; near shrunk for
// conservative culling) ----
float4x4 DsSpotVP(float3 apex, float3 axis, float outerAngle, float range)
{
    const float3 upRef = (abs(dot(axis, float3(0.0f, 1.0f, 0.0f))) > 0.99f)
        ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    const float3 s = normalize(cross(axis, upRef));
    const float3 u = cross(s, axis);
    const float4x4 viewM = float4x4(
        float4(s.x, s.y, s.z, -dot(s, apex)),
        float4(u.x, u.y, u.z, -dot(u, apex)),
        float4(-axis.x, -axis.y, -axis.z, dot(axis, apex)),
        float4(0.0f, 0.0f, 0.0f, 1.0f));
    const float fov = clamp(outerAngle * 2.0f, 0.05f, radians(170.0f));
    const float f = 1.0f / tan(fov * 0.5f);
    const float n = 0.05f;
    const float farP = max(range, n + 0.01f);
    // glm perspectiveRH_ZO rows (Result[col][row]: [3][2] is ROW 2 col 3
    // — the -(far*near)/(far-near) translation; [2][3] is ROW 3 col 2 —
    // the -1 of the w row. Swapping them scales NDC z by ~(f+n)/f and
    // every froxel lands outside [0,1]).
    const float4x4 projM = float4x4(
        float4(f, 0.0f, 0.0f, 0.0f),
        float4(0.0f, f, 0.0f, 0.0f),
        float4(0.0f, 0.0f, farP / (n - farP), -(farP * n) / (farP - n)),
        float4(0.0f, 0.0f, -1.0f, 0.0f));
    return mul(projM, viewM);
}

// AABB of the froxel corners (projected through the light VP) vs the
// light's unit clip box. Conservative: the projected AABB contains the
// projected froxel, so overlap is necessary for intersection (may keep
// lights that only graze — phase 3/4 cull them per pixel).
bool DsFroxelInSpotClip(float4x4 vp, float3 corners[8])
{
    float3 mn = float3(1e30f, 1e30f, 1e30f);
    float3 mx = float3(-1e30f, -1e30f, -1e30f);
    bool anyFront = false;
    [unroll]
    for (int i = 0; i < 8; ++i) {
        const float4 clip = mul(vp, float4(corners[i], 1.0f));
        if (clip.w > 1e-6f) {
            const float3 ndc = clip.xyz / clip.w;
            mn = min(mn, ndc);
            mx = max(mx, ndc);
            anyFront = true;
        }
    }
    if (!anyFront)
        return false; // whole froxel behind the light apex
    const float m = 0.05f;
    return mn.x <= 1.0f + m && mx.x >= -1.0f - m &&
        mn.y <= 1.0f + m && mx.y >= -1.0f - m &&
        mn.z <= 1.0f + m && mx.z >= -m;
}

// Conservative sphere/cone overlap ('Cull That Cone' article, re-derived):
// f(p) = lateral*cos - axial*sin is the signed distance to the infinite
// cone surface (|grad f| = 1), so the sphere is entirely outside laterally
// iff f(center) >= radius; the apex plane and end cap are separate
// axis-range tests. Cull only when provably outside -> false positives ok.
bool DsConeSphereTest(float3 apex, float3 axis, float outerAngle,
    float range, float3 center, float radius)
{
    const float3 c = center - apex;
    const float a = dot(c, axis);
    if (a - radius > range)
        return false; // fully beyond end cap
    if (a + radius < 0.0f)
        return false; // fully behind apex plane
    float so; float co;
    sincos(outerAngle, so, co);
    const float lat = sqrt(max(dot(c, c) - a * a, 0.0f));
    if (lat * co - a * so >= radius)
        return false; // fully outside the infinite cone
    return true;
}

// All bound intersections between one light and the froxel (paper
// 'LightFroxelTests'): cheap sphere/sphere first, then per type.
bool DsLightFroxelTests(ForwardPlusLight L, float4x4 view,
    float3 froxelCenter, float froxelRadius, float3 boxMin, float3 boxMax,
    float3 corners[8])
{
    const float range = (L.range <= 0.0f) ? (float)DS_INFINITE_RANGE : L.range;
    const float3 cView = mul(view, float4(L.position, 1.0f)).xyz;

    // Sphere fully behind the camera plane (view looks down -z): the
    // froxel lives at depths >= near, so it cannot intersect.
    if (cView.z - range > 0.0f)
        return false;

    // Sphere/sphere quick reject.
    const float3 dc = cView - froxelCenter;
    const float rr = range + froxelRadius;
    if (dot(dc, dc) > rr * rr)
        return false;

    if (L.type != 1) {
        // Point: sphere vs froxel AABB (closest point on box).
        const float3 q = clamp(cView, boxMin, boxMax);
        const float3 dq = cView - q;
        return dot(dq, dq) <= range * range;
    }

    // Spot: cone/sphere vs the froxel bounding sphere.
    const float3 axis = normalize(mul(view, float4(L.direction, 0.0f)).xyz);
    if (!DsConeSphereTest(cView, axis, L.outerAngle, range,
        froxelCenter, froxelRadius))
        return false;
    // Debug bisection: cone test passed (candidate count).
    InterlockedAdd(pcDS.counters.Get().dbgSpotCone, 1u);

    // Spot: AABB test in the light clipping space (froxel corners through
    // the spot VP vs the unit box).
    const float4x4 vp = DsSpotVP(cView, axis, L.outerAngle, range);
    if (!DsFroxelInSpotClip(vp, corners))
        return false;
    // Debug bisection: clip test passed.
    InterlockedAdd(pcDS.counters.Get().dbgSpotClip, 1u);
    return true;
}

[numthreads(DS_TILE_GROUP_TILES, DS_TILE_GROUP_TILES, 1)]
void mainCS(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint laneIdx = tid.y * DS_TILE_GROUP_TILES + tid.x; // 0..63

    // ---- Init groupshared (fresh per group dispatch) ----
    if (laneIdx < DS_MAX_LIGHT_GROUPS)
        gsGrpMasks[laneIdx] = 0u;
    GroupMemoryBarrierWithGroupSync();

    const uint2 tileCoord = gid.xy * uint2(DS_TILE_GROUP_TILES, DS_TILE_GROUP_TILES) + tid.xy;
    const bool validTile = all(tileCoord < uint2(pcDS.tileCountX, pcDS.tileCountY));
    uint count = 0;

    if (validTile) {
        const FrameUniforms frame = pcDS.frameUniforms.Get();
        const uint2 vp = (uint2)frame.viewportSize;
        const uint depthIdx = NonUniformResourceIndex(pcDS.depthTextureIndex);

        // ---- Tile min/max depth over the 8x8 texels (sky-filtered) ----
        const uint2 px0 = tileCoord * DS_SHADOW_TILE_PX;
        float minD = 1.0f;   // reversed-Z: small = far
        float maxD = 0.0f;   // large = near
        bool hasSurface = false;
        for (uint y = 0; y < DS_SHADOW_TILE_PX; ++y) {
            for (uint x = 0; x < DS_SHADOW_TILE_PX; ++x) {
                const int2 lp = int2(min(px0 + uint2(x, y), vp - 1u));
                const float d = textures[depthIdx].Load(int3(lp, 0)).r;
                if (d > (float)DS_SKY_DEPTH_EPS) {
                    minD = min(minD, d);
                    maxD = max(maxD, d);
                    hasSurface = true;
                }
            }
        }

        if (hasSurface) {
            const float nearP = max(frame.nearPlane, 1e-4f);
            const float nearestDist = nearP / max(maxD, 1e-6f);
            const float farthestDist = nearP / max(minD, 1e-6f);

            // ---- zBin window over the sorted light buffer ----
            uint lo; uint hi;
            DsZBinRange(pcDS.zbins,
                DsBinFromDist(nearestDist, nearP, frame.farPlane),
                DsBinFromDist(farthestDist, nearP, frame.farPlane),
                pcDS.lightCount, lo, hi);

            // ---- Intersect with the CPU Tile Light Mask (TileGroup) ----
            const uint groupIdx = gid.y * pcDS.groupCountX + gid.x;
            uint lightGroupMask = pcDS.tileMasks.Get().masks[DsGroupMaskAddress(groupIdx)];

            // ---- Froxel geometry (view space) ----
            float3 corners[8];
            float3 boxMin = float3(1e30f, 1e30f, 1e30f);
            float3 boxMax = float3(-1e30f, -1e30f, -1e30f);
            {
                const float x0 = (float(px0.x) / max(frame.viewportSize.x, 1.0f)) * 2.0f - 1.0f;
                const float x1 = (float(px0.x + DS_SHADOW_TILE_PX) / max(frame.viewportSize.x, 1.0f)) * 2.0f - 1.0f;
                const float y0 = (float(px0.y) / max(frame.viewportSize.y, 1.0f)) * 2.0f - 1.0f;
                const float y1 = (float(px0.y + DS_SHADOW_TILE_PX) / max(frame.viewportSize.y, 1.0f)) * 2.0f - 1.0f;
                const float3 d00 = DsUnprojectView(float2(x0, y0), frame.invProj);
                const float3 d10 = DsUnprojectView(float2(x1, y0), frame.invProj);
                const float3 d11 = DsUnprojectView(float2(x1, y1), frame.invProj);
                const float3 d01 = DsUnprojectView(float2(x0, y1), frame.invProj);
                const float3 dirs[4] = { d00, d10, d11, d01 };
                const float depths[2] = { nearestDist, farthestDist };
                [unroll]
                for (int c = 0; c < 4; ++c) {
                    [unroll]
                    for (int e = 0; e < 2; ++e) {
                        // point at view depth D along direction d
                        const float3 p = dirs[c] * (depths[e] / max(-dirs[c].z, 1e-6f));
                        corners[c * 2 + e] = p;
                        boxMin = min(boxMin, p);
                        boxMax = max(boxMax, p);
                    }
                }
            }
            const float3 froxelCenter = 0.5f * (boxMin + boxMax);
            const float froxelRadius = 0.5f * length(boxMax - boxMin);

            // ---- Vet every candidate light (group mask AND zBin window) ----
            while (lightGroupMask != 0u) {
                const uint grp = firstbitlow(lightGroupMask);
                lightGroupMask ^= 1u << grp;
                uint lm = pcDS.tileMasks.Get().masks[DsGroupEntryAddress(groupIdx, grp)]
                    & DsWindowMask(grp, lo, hi);
                uint tileValid = 0u;
                while (lm != 0u) {
                    const uint bit = firstbitlow(lm);
                    lm ^= 1u << bit;
                    const uint lightIdx = grp * 32u + bit;
                    if (lightIdx >= pcDS.lightCount)
                        continue;
                    const ForwardPlusLight L = pcDS.lights.Get().lights[lightIdx];
                    if (DsLightFroxelTests(L, frame.view, froxelCenter,
                        froxelRadius, boxMin, boxMax, corners)) {
                        if (count < DS_MAX_LIGHTS_PER_TILE) {
                            count += 1u;
                            tileValid |= 1u << bit;
                        }
                    }
                }
                if (tileValid != 0u)
                    InterlockedOr(gsGrpMasks[grp], tileValid);
            }
        }
    }
    gsCounts[laneIdx] = count;
    GroupMemoryBarrierWithGroupSync();

    // ---- Thread 0: TileGroup light masks + one slot reservation for the
    // whole TileGroup (listing 1: WaveActiveSum/prefix on consoles; plain
    // groupshared prefix here so wave32 GPUs are safe) ----
    if (laneIdx == 0) {
        const uint groupIdx = gid.y * pcDS.groupCountX + gid.x;
        uint groupMaskOut = 0u;
        [unroll]
        for (uint g = 0; g < DS_MAX_LIGHT_GROUPS; ++g) {
            const uint m = gsGrpMasks[g];
            pcDS.tileGroupMasks.Get().masks[DsGroupEntryAddress(groupIdx, g)] = m;
            if (m != 0u)
                groupMaskOut |= 1u << g;
        }
        pcDS.tileGroupMasks.Get().masks[DsGroupMaskAddress(groupIdx)] = groupMaskOut;

        uint total = 0u;
        [unroll]
        for (uint t = 0; t < 64u; ++t) {
            gsPrefix[t] = total;
            total += gsCounts[t];
        }
        uint base = 0u;
        InterlockedAdd(pcDS.counters.Get().listSlots, total, base);
        // Overflow guard: clamp so every slot+offset is out of range and
        // phases 3/4 skip their stores (degraded, no OOB).
        gsBase = (base + total > DS_MAX_LIST_UINTS) ? DS_MAX_LIST_UINTS : base;
    }
    GroupMemoryBarrierWithGroupSync();

    if (validTile) {
        const uint tileIdx = tileCoord.y * pcDS.tileCountX + tileCoord.x;
        const uint slot = min(gsBase + gsPrefix[laneIdx], DS_MAX_LIST_UINTS);
        pcDS.tileState.Get().states[tileIdx * 2u] = slot;
        pcDS.tileState.Get().states[tileIdx * 2u + 1u] = 0u; // count (phase 3)
    }
}
