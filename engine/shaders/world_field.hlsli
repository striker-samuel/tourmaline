// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldFields — the ONE shared world-space voxel-field sampler.
// Included AFTER common.hlsli (FrameUniforms carries the table BDA;
// volumeTextures[] + samplers[] are the bindless arrays).
//
// Every consumer (volumetric fog, wind particles, future ambient/sim
// effects) calls SampleWorldField / SampleWorldFieldAuto instead of
// re-implementing world->brick conversion, page-table lookup, slot
// decode and filtering. A fog shader must not know how atlas residency
// works; a wind shader must not know fog exists.
//
// CPU mirror: Engine/WorldFields/WorldFieldGpu.h (layout contract, both
// sides pinned by static_asserts + these offset comments) and
// WorldField.cpp's addressing helpers (identical formulas — the
// world-field tests pin negative coordinates, LOD boundaries and exact
// brick edges against both).
//
// GPU table (one storage buffer, BDA = frame.worldFields):
//   +0                  WorldFieldTableHeader (16 B)
//   +16                 WorldFieldGpu fields[fieldCount] (320 B stride)
//   +header.pagesOffset WorldPageGpu pages[] (16 B each, flat pool;
//                      each field level's array addressed by
//                      WorldLevelGpu.pageTableOffset, z-major cells)
//
// Field ids are REGISTRATION ORDER (fog registers first —
// WorldRenderer::InitWorldFields keeps the WORLD_FIELD_ID_* order).
//
// Missing-brick policy: try the requested level, then each coarser one
// (the coarse fallback pages), then the field's DECODED fallbackValue —
// consumers never need their own fallback plumbing.

// ---- constants: MUST match Engine/WorldFields (WorldFieldGpu.h /
// WorldField.h — editing either side recompiles everything via the
// SHADER_HEADERS glob) ----
#define WORLD_FIELD_MAX_FIELDS 8
#define WORLD_FIELD_MAX_LODS 4
#define WORLD_FIELD_BRICK_INTERIOR 64 // interior voxel count per axis
#define WORLD_FIELD_BRICK_STORED 68   // interior + 2-texel halo per side
#define WORLD_FIELD_BRICK_BORDER 2

// WorldFieldEncoding (Engine/WorldFields/WorldField.h)
#define WORLD_FIELD_ENC_LINEAR 0
#define WORLD_FIELD_ENC_SQRT_UNORM 1
#define WORLD_FIELD_ENC_SIGNED_NORMALIZED 2

// Field ids (registration order — the App registers fog, then fog-GI,
// then wind).
#define WORLD_FIELD_ID_FOG 0
#define WORLD_FIELD_ID_FOG_GI 1
#define WORLD_FIELD_ID_WIND 2

// ---- struct mirrors (offsets pinned; see WorldFieldGpu.h) ----

struct WorldPageGpu
{
    // xyz = ABSOLUTE brick coord (the stale-toroidal guard rechecks it),
    // w = physical atlas slot (-1 = not resident this frame).
    int4 brickSlot;                                           // 0
};                                                            // 16 B

struct WorldLevelGpu
{
    int4 windowMinBrick;   // 0   xyz = min brick coord of the window; w pad
    uint4 windowDim;       // 16  xyz = per-axis entry counts (power of two)
    uint pageTableOffset;  // 32  byte offset of this level's pages in the table
    float voxelSizeMeters; // 36  voxel edge at this LOD (base * 2^lod)
    uint2 reserved;        // 40
};                          // 48 B

struct WorldFieldGpu
{
    float4 originMeters;     // 0   xyz = world min corner of brick (0,0,0)
    float4 decodeScale;      // 16
    float4 decodeBias;       // 32
    float4 fallbackValue;    // 48  DECODED fallback (world units)
    uint4 atlasSlots;       // 64  xyz = physical slot grid (atlas dims = slots * 68)
    uint4 meta;             // 80  x = atlas bindless index (0xffffffff = none),
                            //     y = encoding, z = levelCount, w = flags
    WorldLevelGpu levels[4]; // 96  WORLD_FIELD_MAX_LODS
    uint4 reserved[2];      // 288
};                          // 320 B

struct WorldFieldTableHeader
{
    // x = fieldCount, y = fieldStride (bytes), z = pagesOffset, w = pad.
    uint4 h;                                                  // 0
};                                                            // 16 B

// ---- raw table loads (arbitrary-offset BDA reads; the
// probePremixSel pattern) ----

WorldFieldTableHeader WorldFieldLoadHeader(uint64_t tableBda)
{
    return vk::BufferPointer<WorldFieldTableHeader>(tableBda).Get();
}

WorldFieldGpu WorldFieldLoadField(uint64_t tableBda, uint fieldId, uint fieldStride)
{
    return vk::BufferPointer<WorldFieldGpu>(
        tableBda + uint64_t(16u + fieldId * fieldStride)).Get();
}

WorldPageGpu WorldFieldLoadPage(uint64_t tableBda, uint pageTableOffset, uint byteIndex)
{
    return vk::BufferPointer<WorldPageGpu>(tableBda + uint64_t(pageTableOffset + byteIndex)).Get();
}

// Encoded texel -> attribute. Called AFTER interpolation: hardware
// trilinear filters the encoded range, then this decodes (signed wind
// vectors blend correctly as SNORM first; sqrt-density's encoded-domain
// filtering is a deliberate precision trade, unit-tested against a
// dense reference on the CPU side).
float4 WorldFieldDecode(float4 encoded, const WorldFieldGpu f)
{
    if (f.meta.y == WORLD_FIELD_ENC_SQRT_UNORM)
        return f.decodeScale * (encoded * encoded) + f.decodeBias;
    return f.decodeScale * encoded + f.decodeBias; // Linear + SignedNormalized
}

// One level attempt: window bounds -> toroidal lookup -> stale/absent
// guard -> slot decode -> halo-padded atlas UVW -> trilinear -> decode.
// false = miss (outside the window, page not resident, stale toroidal
// tag, or no atlas) — the caller walks coarser, then falls back.
bool WorldFieldTryLevel(uint64_t tableBda, const WorldFieldGpu f, const WorldLevelGpu lv,
    float3 worldPos, out float4 result)
{
    if (f.meta.x == 0xffffffffu)
        return false; // no physical atlas (creation degraded) — fallback

    // Continuous voxel cell. floor() everywhere: negative world coords
    // map to NEGATIVE bricks (truncation puts every negative cell one
    // brick off — the classic sparse-paging bug).
    const float3 cell = (worldPos - f.originMeters.xyz) / lv.voxelSizeMeters;
    const int3 brick = int3(floor(cell / 64.0)); // exact: floor made integers
    const float3 local = cell - float3(brick) * 64.0;

    // Inside this level's window?
    const int3 rel = brick - lv.windowMinBrick.xyz;
    if (any(rel < 0) || any(rel >= int3(lv.windowDim.xyz)))
        return false;

    // Toroidal cell (power-of-two dims -> bitmask wrap; uint3(int3) is
    // two's complement, so brick -1 wraps to dim-1 exactly like the CPU).
    const uint3 ring = uint3(brick) & (lv.windowDim.xyz - 1u);
    const uint byteIndex = ((ring.z * lv.windowDim.y + ring.y)
        * lv.windowDim.x + ring.x) * 16u;
    const WorldPageGpu page = WorldFieldLoadPage(tableBda, lv.pageTableOffset, byteIndex);

    // Residency + the stale-toroidal guard: a wrapped-around cell may
    // still hold an entry tagged with a DIFFERENT absolute brick — that
    // entry is a previous window's ghost, not this brick's data.
    if (page.brickSlot.w < 0 || any(page.brickSlot.xyz != brick))
        return false;

    // Slot -> atlas XYZ (slot grid from the field).
    const uint s = uint(page.brickSlot.w);
    const uint sx = s % f.atlasSlots.x;
    const uint sy = (s / f.atlasSlots.x) % f.atlasSlots.y;
    const uint sz = s / (f.atlasSlots.x * f.atlasSlots.y);

    // Stored-texel position: slot origin (stored units) + interior local
    // + halo skip. The 2-texel halo means edge-filtered samples read the
    // NEIGHBOR brick's data as stored INSIDE this slot — correct
    // world-space filtering with zero slot-to-slot atlas bleed.
    const float3 atlasVoxel = float3(sx, sy, sz) * 68.0 + local + 2.0;
    const float3 uvw = atlasVoxel / (f.atlasSlots.xyz * 68.0);

    // NonUniformResourceIndex: the shared API must stay correct for a
    // per-pixel divergent fieldId (a wave-uniform caller pays a little
    // descriptor-path overhead; hand-inline if that ever measures).
    Texture3D<float4> atlas = volumeTextures[NonUniformResourceIndex(f.meta.x)];
    // samplers[2] = linear clamp-to-edge (the cross-backend slot
    // contract); mip 0 — virtual LODs are resident bricks, never atlas mips.
    const float4 encoded = atlas.SampleLevel(samplers[2], uvw, 0.0);

    result = WorldFieldDecode(encoded, f);
    return true;
}

// Sample a field at a specific LOD; walk coarser on misses; the decoded
// fallback when nothing resolves. Returns 0 when the subsystem is off
// (frame.worldFields == 0 / count 0 / id out of range) — gate on
// frame.worldFieldCount when zero-vs-fallback matters.
float4 SampleWorldField(const FrameUniforms frame, uint fieldId, float3 worldPos, uint lod)
{
    if (frame.worldFields == 0u || frame.worldFieldCount == 0u
        || fieldId >= frame.worldFieldCount)
        return 0.0.xxxx;

    const WorldFieldTableHeader h = WorldFieldLoadHeader(frame.worldFields);
    const WorldFieldGpu f = WorldFieldLoadField(frame.worldFields, fieldId, h.h.y);
    if (f.meta.x == 0xffffffffu)
        return f.fallbackValue; // degraded pool: never touch the atlas

    uint l = min(lod, f.meta.z - 1u); // beyond the chain = coarsest
    for (; l < f.meta.z; ++l) {
        float4 result;
        if (WorldFieldTryLevel(frame.worldFields, f, f.levels[l], worldPos, result))
            return result;
    }
    return f.fallbackValue;
}

// Sample at whatever level best matches a desired voxel size: the
// FINEST level whose voxels are <= the request (most detail that still
// satisfies it); a request finer than every level takes the finest
// (closest from above). Same fallback walk as SampleWorldField.
float4 SampleWorldFieldAuto(const FrameUniforms frame, uint fieldId,
    float3 worldPos, float desiredVoxelSizeMeters)
{
    if (frame.worldFields == 0u || frame.worldFieldCount == 0u
        || fieldId >= frame.worldFieldCount)
        return 0.0.xxxx;

    const WorldFieldTableHeader h = WorldFieldLoadHeader(frame.worldFields);
    const WorldFieldGpu f = WorldFieldLoadField(frame.worldFields, fieldId, h.h.y);

    uint pick = 0u; // finest default (a request finer than every level)
    for (uint l = 0; l < f.meta.z; ++l) {
        if (f.levels[l].voxelSizeMeters <= desiredVoxelSizeMeters) {
            pick = l;
            break;
        }
    }
    return SampleWorldField(frame, fieldId, worldPos, pick);
}

// ---------------------------------------------------------------------------
// The paired fog-GI sampler (the BC6H radiance + BC4 coverage pair — see
// WORLD_FIELD_ID_FOG_GI): resolves the world-field page table ONCE and
// samples both fields' atlases through the same brick/slot/uvw. Returns
// the radiance, the coverage, and whether a resident pair was found.
// The coverage channel distinguishes a genuinely dark covered voxel
// (radiance ~0, coverage ~1) from an uncovered position (coverage ~0 ->
// the caller uses the sky mean) — BC6H RGB cannot carry that in alpha.
// ---------------------------------------------------------------------------
struct FogGiSample
{
    float3 radiance;    // the baked isotropic mean (0.124959 * SG amps)
    float coverage;     // the baked probe coverage weight (0 = sky fall)
    bool resident;       // a resident brick pair served the sample
};

FogGiSample SampleFogGi(const FrameUniforms frame, float3 worldPos, uint lod)
{
    FogGiSample result;
    result.radiance = float3(0.0, 0.0, 0.0);
    result.coverage = 0.0;
    result.resident = false;
    if (frame.worldFields == 0u || frame.worldFieldCount == 0u
        || WORLD_FIELD_ID_FOG_GI >= frame.worldFieldCount)
        return result;

    // Resolve the page table + brick once (shared across both fields —
    // they use the same world addressing, brick coordinates and halos).
    const WorldFieldTableHeader h = WorldFieldLoadHeader(frame.worldFields);
    const WorldFieldGpu f = WorldFieldLoadField(
        frame.worldFields, WORLD_FIELD_ID_FOG_GI, h.h.y);
    if (f.meta.x == 0xffffffffu)
        return result; // no physical atlas

    uint l = min(lod, f.meta.z - 1u);
    for (; l < f.meta.z; ++l) {
        const WorldLevelGpu lv = f.levels[l];
        const float3 cell = (worldPos - f.originMeters.xyz) / lv.voxelSizeMeters;
        const int3 brick = int3(floor(cell / 64.0));
        const float3 local = cell - float3(brick) * 64.0;
        const int3 rel = brick - lv.windowMinBrick.xyz;
        if (any(rel < int3(0, 0, 0)) || any(rel >= int3(lv.windowDim.xyz)))
            continue;
        const uint3 ring = uint3(brick) & (lv.windowDim.xyz - 1u);
        const uint byteIndex = ((ring.z * lv.windowDim.y + ring.y)
            * lv.windowDim.x + ring.x) * 16u;
        const WorldPageGpu page = WorldFieldLoadPage(
            frame.worldFields, lv.pageTableOffset, byteIndex);
        if (page.brickSlot.w < 0 || any(page.brickSlot.xyz != brick))
            continue; // stale toroidal entry or not resident

        // Slot -> atlas XYZ (the fog-GI field's own slot grid).
        const uint s = uint(page.brickSlot.w);
        const uint sx = s % f.atlasSlots.x;
        const uint sy = (s / f.atlasSlots.x) % f.atlasSlots.y;
        const uint sz = s / (f.atlasSlots.x * f.atlasSlots.y);
        const float3 atlasVoxel = float3(sx, sy, sz) * 68.0 + local + 2.0;
        const float3 uvw = atlasVoxel / (f.atlasSlots.xyz * 68.0);

        // Radiance: one trilinear BC6H fetch (RGB = the baked mean).
        Texture3D<float4> atlas = volumeTextures[
            NonUniformResourceIndex(f.meta.x)];
        const float4 gi = atlas.SampleLevel(samplers[2], uvw, 0.0);
        result.radiance = gi.rgb;
        result.resident = true;

        // Coverage: the SEPARATE BC4 coverage field (field id 2). It has
        // its OWN page table + slot assignments (independent LRU from
        // the radiance field — the same brick coordinate maps to
        // different physical slots), so we resolve it through its own
        // SampleWorldField walk rather than reusing the radiance field's
        // uvw (which would address the WRONG slot in the coverage
        // atlas). SampleWorldField handles the page-table lookup, the
        // coarser-walk, and the fallback (not resident = fallbackValue
        // = 0 = uncovered = the sky fallback in the fog shader).
        if (WORLD_FIELD_ID_FOG_GI + 1u < frame.worldFieldCount) {
            const float4 cov = SampleWorldField(
                frame, WORLD_FIELD_ID_FOG_GI + 1u, worldPos, l);
            result.coverage = cov.r;
        }

        // If the coverage field is absent, treat resident as covered
        // (the radiance IS the data — no separate coverage baked).
        if (WORLD_FIELD_ID_FOG_GI + 1u >= frame.worldFieldCount)
            result.coverage = 1.0;

        return result;
    }
    // No level resolved: not resident (the caller uses the sky).
    return result;
}
