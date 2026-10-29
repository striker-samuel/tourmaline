// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Forward+ tile light culling (point/spot/probe; directional stays on CSM
// path) + decal culling (projected splats share the light tiling) + convex
// GI-volume culling (per-tile bitmask; sg_gi_volume.hlsli consumes it).
//
// Reflection probes ride the light buffer as type-2 entries
// (kForwardPlusProbe: position = center, range = influence + fade): the
// generic sphere leg below culls them with zero special cases, and the
// lit shader's type filter picks them back out of the same tile lists.
//
// One thread group per tile ([1,1,1], dispatched tileCountX x tileCountY).
// Each tile:
//   1. loads its 16x16 depth texels, tracks NDC min/max (reversed-Z:
//      1 = near, 0 = far/sky),
//   2. unprojects tile corners via invProj -> view-space side planes,
//   3. sphere-tests every light against the 4 tile side planes,
//   4. SURFACE list: lights passing the side planes AND the tile depth
//      slab are RELEVANCE-SORTED (key = distance from the light center
//      to the slab; spots pinned ahead of points) and the best
//      maxLightsPerTile survive — overflow pops the FARTHEST candidates,
//      a graceful distance fade instead of the old first-come hard edge
//      that read as tile seams under dense crowds. The stored count is
//      the sorted list length (never beyond the written entries).
//   4b. FOG list: SEPARATE per-tile lists for the volumetric march
//      (fogTileCounts/fogTileIndices, stride maxFogPerTile). Candidacy
//      ignores the depth slab — march samples live in front of the
//      nearest opaque surface — and only cuts beyond the march range
//      (fogMaxDistance); same relevance sort, spots pinned; probes never
//      ride (scalar march). The fog walk then sees a BOUNDED,
//      seam-free list (the march-range candidacy is per-tile constant,
//      so neighboring tiles no longer disagree at depth slabs).
//   5. sphere-tests every decal (ForwardPlusDecal bounding sphere) the
//      same way and writes parallel per-tile decal lists,
//   6. sphere-tests every convex GI volume's fade-expanded AABB against
//      the tile side planes and packs the survivors into a per-tile
//      bitmask (one bit per volume — the volumes blend by ORDERED
//      priority, so a capped index list could not represent them; the
//      <= 8 volume budget makes a mask the natural encoding).
//
// Depth buffer conventions (must match app):
//   - infinite reversed-Z projection, clear 0 (far), GreaterEqual.
//   - view distance from NDC depth d: dist = near / max(d, eps).
//   - NDC mapping with the engine viewport (y down in framebuffer):
//       ndcX = (px / w) * 2 - 1, ndcY = (py / h) * 2 - 1.

#define FORWARDPLUS_PASS
#include "common.hlsli"

struct ForwardPlusCullPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<ForwardPlusLightBuffer> lights;
    vk::BufferPointer<ForwardPlusTileCountBuffer> tileCounts;
    vk::BufferPointer<ForwardPlusTileIndexBuffer> tileIndices;
    uint depthTextureIndex;
    uint tileCountX;
    uint tileCountY;
    uint lightCount;
    uint tileSize;
    uint maxLightsPerTile;
    // Per-tile convex-volume bitmask output (the tail slot keeps every
    // field offset identical to the old pad0/pad1 layout).
    vk::BufferPointer<GiVolumeTileMaskBuffer> volumeMasks;
    // Per-tile FOG lists (see the header + the C++ mirror in
    // Engine/Rendergraph/forwardplus.h): march-range candidacy for the
    // volumetric pass, separate buffers from the surface lists.
    vk::BufferPointer<ForwardPlusFogTileCountBuffer> fogTileCounts;
    vk::BufferPointer<ForwardPlusFogTileIndexBuffer> fogTileIndices;
    float fogMaxDistance;   // march-range far cut
    uint maxFogPerTile;     // fog row stride (matches the volumetric push)
    uint fogListsWanted;    // 0 = skip the fog track (vol pass off — no consumer)
};
// NOTE: no static_assert here – this DXC (HLSL 2018 mode) rejects it at
// global scope. Size is checked on the C++ side (88 bytes).

[[vk::push_constant]]
ConstantBuffer<ForwardPlusCullPush> pcCull;

// Unproject NDC -> view space (exact, flip/reversed-Z agnostic).
float3 UnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

[numthreads(1, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID)
{
    uint tileX = groupId.x;
    uint tileY = groupId.y;
    if (tileX >= pcCull.tileCountX || tileY >= pcCull.tileCountY)
        return;
    uint tileIdx = tileY * pcCull.tileCountX + tileX;
    if (tileIdx >= (uint)MAX_FP_TILES)
        return;

    FrameUniforms frame = pcCull.frameUniforms.Get();
    float viewportW = max(frame.viewportSize.x, 1.0f);
    float viewportH = max(frame.viewportSize.y, 1.0f);
    uint tileSize = max(pcCull.tileSize, 1u);

    uint px0 = tileX * tileSize;
    uint py0 = tileY * tileSize;
    uint px1 = min(px0 + tileSize, (uint)viewportW);
    uint py1 = min(py0 + tileSize, (uint)viewportH);

    // ---- Depth min/max over the tile (NDC z, reversed: 1 near) ----
    uint depthIdx = NonUniformResourceIndex(pcCull.depthTextureIndex);
    float minD = 1.0f;
    float maxD = 0.0f;
    for (uint py = py0; py < py1; ++py)
    {
        for (uint px = px0; px < px1; ++px)
        {
            float d = textures[depthIdx].Load(int3((int)px, (int)py, 0)).r;
            minD = min(minD, d);
            maxD = max(maxD, d);
        }
    }

    // ---- Tile corners in NDC (framebuffer y down -> ndcY down) ----
    float x0 = (float(px0) / viewportW) * 2.0f - 1.0f;
    float x1 = (float(px1) / viewportW) * 2.0f - 1.0f;
    float y0 = (float(py0) / viewportH) * 2.0f - 1.0f;
    float y1 = (float(py1) / viewportH) * 2.0f - 1.0f;

    float4x4 invProj = frame.invProj;
    // Side planes go through the camera origin; use near corners (ndc=1).
    float3 c00 = UnprojectView(float2(x0, y0), 1.0f, invProj);
    float3 c10 = UnprojectView(float2(x1, y0), 1.0f, invProj);
    float3 c11 = UnprojectView(float2(x1, y1), 1.0f, invProj);
    float3 c01 = UnprojectView(float2(x0, y1), 1.0f, invProj);

    // Inward-facing side planes through the camera origin (plane dist = 0).
    // NDC y is framebuffer-y-down while view space is y-up, so the ndc row
    // y0 (top of the tile on screen) unprojects to the TOP edge in view
    // space: c00=top-left, c10=top-right, c11=bottom-right, c01=bottom-left.
    // Each cross() below is ordered so the normal points INTO the frustum
    // (verified by hand: top->down, bottom->up, left->right, right->left);
    // a sphere is outside iff dot(n, center) < -radius.
    float3 planes[4];
    planes[0] = normalize(cross(c00, c10)); // top
    planes[1] = normalize(cross(c11, c01)); // bottom
    planes[2] = normalize(cross(c01, c00)); // left
    planes[3] = normalize(cross(c10, c11)); // right

    // Depth range -> view-space distances (reversed-Z: dist = near / d).
    float nearP = max(frame.nearPlane, 1e-4f);
    float nearestDist = nearP / max(maxD, 1e-6f);   // closest surface
    float farthestDist = nearP / max(minD, 1e-6f);  // farthest (sky -> huge)

    // ---- Cull lights + probes (split per-tile slots) ----
    // One pass over the mixed entry range: type != probe packs the LIGHT
    // slots [0..perTile), type == probe packs the PROBE slots
    // [perTile..perTile+MAX_FP_PROBE_TILE_SLOTS) — stride
    // perTile + MAX_FP_PROBE_TILE_SLOTS matches the C++ allocation.
    // Split slots are load-bearing: sharing one [0..perTile) budget let
    // light-saturated tiles pop their probes, flipping specular IBL to
    // the global fallback cubemap (chrome ball "breaks" when a light grid
    // comes on). Overflow now only ever pops within its own class:
    // probes beyond the tail slots (first-come = upload order, pinned
    // probes first). LIGHT overflow pops by RELEVANCE instead (sorted
    // insertion below): the farthest candidates go first — the old
    // first-come pop read as hard tile seams under dense crowds because
    // neighboring tiles kept different arbitrary subsets.
    // counts: low 16 = SORTED light list length (never beyond the
    // written entries), high 16 = TRUE (unclamped) probe count.
    uint lightCount = min(pcCull.lightCount, (uint)MAX_FP_LIGHTS);
    // Effective slot capacity via the SHARED clamp chain
    // (common.hlsli FpTileLightSlots): every surface-list consumer
    // derives its row stride from the same function, so producer and
    // consumer always address the same rows whatever the configured
    // maxLightsPerTile. A desc raise beyond the sort capacity clamps
    // here and the stored count follows.
    const uint maxPerTile = FpTileLightSlots(pcCull.maxLightsPerTile);
    const uint stride = maxPerTile + (uint)MAX_FP_PROBE_TILE_SLOTS;
    // Fog row stride: the pushed desc value clamped to the same sort
    // capacity (the volumetric consumer clamps identically, so both
    // sides address the same rows of the fog buffers).
    const uint fogStride = min(pcCull.maxFogPerTile, (uint)MAX_FP_TILE_SORT_SLOTS);
    // The fog track's consumer gate: 0 = the volumetric pass is off this
    // frame (skip the per-light fog candidacy + the writeback entirely —
    // real per-tile work saved, not dead stores).
    const bool fogLists = pcCull.fogListsWanted != 0u;
    uint lcount = 0;  // sorted surface list length
    uint pcount = 0;  // true probe count (first-come packing)
    uint fcount = 0;  // sorted fog list length

    // Relevance-sorted tile list scratch (surface + fog). Written back
    // once after the light loop; the sort is an insertion that evicts the
    // WORST (largest key) entry once full.
    uint sIdx[MAX_FP_TILE_SORT_SLOTS];
    float sKey[MAX_FP_TILE_SORT_SLOTS];
    uint fIdx[MAX_FP_TILE_SORT_SLOTS];
    float fKey[MAX_FP_TILE_SORT_SLOTS];

    // Spot pin: cone lights sort ahead of points unconditionally (the
    // pre-sort contract — spots never lose their slots to the crowd;
    // they are also few, so their own order collapsing to buffer order
    // among themselves is the old first-come semantics). 1e6 dwarfs any
    // real view distance (sky-tile slabs unproject to ~1e5).
    const float kSpotPin = 1e6;

    float4x4 view = frame.view;
    for (uint li = 0; li < lightCount; ++li)
    {
        ForwardPlusLight L = pcCull.lights.Get().lights[li];
        float range = L.range <= 0.0f ? 1e6f : L.range;

        // View-space center. Skip lights behind the camera:
        // view z positive = behind (camera looks down -z).
        float3 cView = mul(view, float4(L.position, 1.0f)).xyz;
        if (cView.z > range)
            continue;

        // Side planes (origin planes, outward normals) — shared by the
        // surface and fog tracks below.
        bool outside = false;
        [unroll]
        for (int p = 0; p < 4; ++p)
        {
            if (dot(planes[p], cView) < -range)
            {
                outside = true;
                break;
            }
        }
        if (outside)
            continue;

        const float d = -cView.z; // light center view distance
        const float spotPin = (L.type == 1) ? -kSpotPin : 0.0f;

        if (L.type == kForwardPlusProbe) {
            // Probes: surface specular IBL only — slab candidacy, plain
            // first-come packing (their own tail slots).
            if (-cView.z + range >= nearestDist && -cView.z - range <= farthestDist) {
                if (pcount < (uint)MAX_FP_PROBE_TILE_SLOTS)
                    pcCull.tileIndices.Get().indices[tileIdx * stride + maxPerTile + pcount] = li;
                pcount++;
            }
            continue;
        }

        // ---- Surface track (slab candidacy + sorted insertion) ----
        // The slab cuts are conservative for every OPAQUE pixel of the
        // tile (pixels live inside the slab by construction), unchanged
        // from the classic cull. Relevance key = distance from the light
        // CENTER to the slab interval: 0 = the center sits inside the
        // slab (the tile's pixels pass right by the light); larger = the
        // light only reaches in from farther out, so it pops first.
        if (-cView.z + range >= nearestDist && -cView.z - range <= farthestDist) {
            const float key = max(max(nearestDist - d, d - farthestDist), 0.0f) + spotPin;
            // Insert: append while free, else evict the worst (largest
            // key) when this candidate beats it. Strict '<' = stable ties
            // (buffer order among equals — spots first).
            if (lcount < maxPerTile) {
                sIdx[lcount] = li;
                sKey[lcount] = key;
                lcount++;
            } else {
                float worst = sKey[0];
                uint worstI = 0u;
                [loop]
                for (uint w = 1u; w < maxPerTile; ++w) {
                    if (sKey[w] > worst) { worst = sKey[w]; worstI = w; }
                }
                if (key < worst) {
                    sIdx[worstI] = li;
                    sKey[worstI] = key;
                }
            }
        }

        // ---- Fog track (march-range candidacy + sorted insertion) ----
        // NO slab cuts: a march sample lives outside the slab whenever
        // it is in the air between the camera and the tile's nearest
        // surface, and a light sitting entirely in front of that surface
        // still lights that air. Cutting on the slab split the fog on the
        // tile grid at depth discontinuities (neighboring tiles' slabs
        // disagree). The single cut: the light's nearest face beyond the
        // march range (fogMaxDistance) can never reach a sample in
        // [0, fogMaxDistance]. Relevance key = distance from the center
        // to the march interval [0, fogMaxDistance] — same shape as the
        // surface key. GATED on fogListsWanted: the volumetric pass's
        // consumer gate (the per-light fog candidacy + sorted insertion
        // is real work per tile, not free dead stores when fog is off).
        if (fogLists && d - range <= pcCull.fogMaxDistance) {
            const float key = max(max(-d, d - pcCull.fogMaxDistance), 0.0f) + spotPin;
            if (fcount < fogStride) {
                fIdx[fcount] = li;
                fKey[fcount] = key;
                fcount++;
            } else {
                float worst = fKey[0];
                uint worstI = 0u;
                [loop]
                for (uint w = 1u; w < fogStride; ++w) {
                    if (fKey[w] > worst) { worst = fKey[w]; worstI = w; }
                }
                if (key < worst) {
                    fIdx[worstI] = li;
                    fKey[worstI] = key;
                }
            }
        }
    }

    // ---- Write back the sorted surface list ----
    [loop]
    for (uint s = 0u; s < lcount; ++s)
        pcCull.tileIndices.Get().indices[tileIdx * stride + s] = sIdx[s];
    // Packed counts: lights low 16 bits (sorted length), probes high 16
    // (true count — the specular walk min()s it with the slot budget).
    // The light loops mask the low word, the specular walk shifts.
    pcCull.tileCounts.Get().counts[tileIdx] = (lcount & 0xFFFFu) | (pcount << 16);

    // ---- Write back the sorted fog list (UNCONDITIONAL count store —
    // fresh 0 with no candidates, same discipline as the decal counts;
    // SKIPPED entirely when fog is off — no consumer reads the lists) ----
    if (fogLists) {
        [loop]
        for (uint s = 0u; s < fcount; ++s)
            pcCull.fogTileIndices.Get().indices[tileIdx * fogStride + s] = fIdx[s];
        pcCull.fogTileCounts.Get().counts[tileIdx] = fcount;
    }

    // ---- Cull decals (same conservative bounding-sphere test as the
    // lights: tile side planes + depth range). Decals share the light
    // tiling; the counts store is UNCONDITIONAL (fresh 0 with no decals)
    // so the shading pass never iterates stale entries. Under deferred
    // shadows the app pushes lights+probes (the DS lists own the lights,
    // the light leg re-culls them as dead work) — the decal leg below
    // still runs, so decals stay alive in both shading paths. Reflection
    // probes need no leg of their own: they ride the light leg above as
    // type-2 entries (position = center, range = influence + fade).
    uint decalCount = min(frame.decalCount, (uint)MAX_FP_DECALS);
    // decalPerTile is the WRITE gate (the shader-side per-tile capacity,
    // MAX_FP_DECALS_PER_TILE). The ROW STRIDE is the desc's
    // decalMaxPerTile — the SAME value the C++ allocation
    // (tiles * maxDecalsPerTile) and the shading consumer
    // (default_lit_body.hlsli ApplyForwardPlusDecals) index with, so
    // producer and consumer address the same rows whatever the desc.
    // (The old code used the CLAMPED value here while the consumer used
    // the raw one — a desc beyond 16 made the two sides read different
    // rows.)
    uint decalPerTile = min(frame.decalMaxPerTile, (uint)MAX_FP_DECALS_PER_TILE);
    const uint decalStride = max(frame.decalMaxPerTile, 1u);
    uint dcount = 0;
    for (uint di = 0; di < decalCount; ++di)
    {
        ForwardPlusDecal D = frame.decals.Get().decals[di];
        float radius = max(D.bounds.w, 0.0f);
        if (radius <= 0.0f)
            continue;

        float3 cView = mul(view, float4(D.bounds.xyz, 1.0f)).xyz;
        if (cView.z > radius)
            continue;
        if (-cView.z + radius < nearestDist)
            continue;
        if (-cView.z - radius > farthestDist)
            continue;

        bool dOutside = false;
        [unroll]
        for (int p = 0; p < 4; ++p)
        {
            if (dot(planes[p], cView) < -radius)
            {
                dOutside = true;
                break;
            }
        }
        if (dOutside)
            continue;

        // Row stride is the desc's decalMaxPerTile (matches the
        // builder's tiles * maxDecalsPerTile allocation and the
        // consumer); decalPerTile only gates how many entries are
        // written into the row.
        if (dcount < decalPerTile)
            frame.decalTileIndices.Get().indices[tileIdx * decalStride + dcount] = di;
        dcount++;
    }
    frame.decalTileCounts.Get().counts[tileIdx] = dcount;

    // ---- Cull GI volumes into the per-tile bitmask ----
    // A volume reaches a tile when the bounding sphere of its
    // fade-expanded AABB intersects the tile's side planes (the
    // behind-camera reject included; the sphere is conservative over
    // the AABB — the per-pixel AABB + plane tests in SgGiVolumeWeight make
    // the final call). NO depth-range cut, unlike the lights: fog-march
    // samples live IN FRONT of the nearest opaque surface (the march
    // starts at the camera), so the near-depth cut would wrongly drop
    // volumes that light the empty space before the wall; surface
    // shading only pays a slightly wider mask instead. The store is
    // UNCONDITIONAL (fresh 0 with no volumes / GI volumes off — same
    // discipline as the decal counts), so the shading passes never see
    // stale bits. Bit v = volume v (upload order = compositing priority
    // order, sg_gi_volume.hlsli).
    uint volBits = 0u;
    uint giVolumeCount = min(frame.giVolumeCount, (uint)SG_GI_VOLUME_MAX);
    for (uint vi = 0; vi < giVolumeCount; ++vi)
    {
        const GiVolumeGpu V = frame.giVolumes.Get().volumes[vi];
        const float3 c = 0.5 * (V.aabbMin.xyz + V.aabbMax.xyz);
        const float r = 0.5 * length(V.aabbMax.xyz - V.aabbMin.xyz);
        float3 cView = mul(view, float4(c, 1.0f)).xyz;
        if (cView.z > r)
            continue; // entirely behind the camera
        bool vOutside = false;
        [unroll]
        for (int p = 0; p < 4; ++p)
        {
            if (dot(planes[p], cView) < -r)
            {
                vOutside = true;
                break;
            }
        }
        if (!vOutside)
            volBits |= 1u << vi;
    }
    pcCull.volumeMasks.Get().masks[tileIdx] = volBits;
}
