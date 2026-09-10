#pragma once
// Deferred Shadows — GPU buffer types + constants only.
//
// Zero-dependency fragment: included by common.hlsli BEFORE FrameUniforms
// (which carries typed pointers to these buffers). The push struct and
// helpers that need FrameUniforms / ForwardPlusLight live in
// deferredshadow.hlsli, included after them. See deferredshadow.hlsli for
// the pipeline overview and the buffer layout notes.

#define DS_SHADOW_TILE_PX 8            // Shadow Tile = 8x8 pixels
#define DS_TILE_GROUP_TILES 8          // TileGroup = 8x8 Shadow Tiles
#define DS_TILE_GROUP_PX 64            // 8 * 8
#define DS_ZBIN_COUNT 1024
#define DS_MAX_LIGHT_GROUPS 32         // MAX_FP_LIGHTS (1024) / 32
#define DS_MAX_TILE_GROUPS 4096        // 64x64px groups (covers 4K)
#define DS_MAX_TILES 131072            // 8px Shadow Tiles (covers 4K)
#define DS_MAX_LIST_UINTS 4194304      // shared light list (16 MB)
#define DS_MAX_VIS_SLOTS 262144        // 64B each (16 MB)
#define DS_MAX_LIGHTS_PER_TILE 96      // hard per-tile cap (safety)
#define DS_VIS_WORDS_PER_SLOT 16       // 64B / 4B
#define DS_LIGHT_IDX_BITS 10
#define DS_SLOT_ALL_LIT 0x3FFFFFu      // 22-bit fully-lit flag
#define DS_SKY_DEPTH_EPS 1e-6          // reversed-Z clear 0 = sky/far
#define DS_INFINITE_RANGE 1e6          // matches C++ kDsInfiniteRange

// ---- GPU buffer types (fixed size, per-flight slices) ----

struct DsZBinBuffer
{
    uint4 bins[DS_ZBIN_COUNT]; // x=minIdx y=maxIdx z=rangedMin w=rangedMax
};

struct DsTileLightMaskBuffer
{
    uint masks[DS_MAX_TILE_GROUPS * (1 + DS_MAX_LIGHT_GROUPS)];
};

struct DsTileStateBuffer
{
    // 4 uints per Shadow Tile: {slotID, count, approxCount, earlyCount}.
    // count is the EARLY count after phase 3 and the FINAL count after
    // phase 4; approxCount (phase 2 reservation) and earlyCount (phase 3)
    // persist for the debug visualizers (paper 5.4: approx vs accurate).
    uint states[DS_MAX_TILES * 4];
};

struct DsLightListBuffer
{
    uint entries[DS_MAX_LIST_UINTS];
};

struct DsVisibilityBuffer
{
    uint words[DS_MAX_VIS_SLOTS * DS_VIS_WORDS_PER_SLOT]; // 4 pixels/uint
};

struct DsCounters
{
    uint listSlots;   // uints reserved in the light list (flexible alloc)
    uint visSlots;    // 64B visibility slots allocated
    // Debug/telemetry classification histogram (phase 3/4):
    uint clsAllLit;   // (tile,light) pairs fully lit (flag entry)
    uint clsAllShadow; // pairs fully in shadow (dropped from the list)
    uint clsPenumbra; // pairs with a VisibilityBuffer slot
    uint sumEarly;    // total early-list entries (phase 3)
    uint sumFinal;    // total final-list entries (phase 4)
    // Phase-2 spot-test bisection (debug): cone/clip candidates kept.
    uint dbgSpotCone;
    uint dbgSpotClip;
    uint pad;
};
