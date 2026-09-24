// placement_common.hlsli — GPU mirror of placement_ids.h + placement_point.h.
// Bit-identical 32-bit integer hashing (plain uint math, no Int64 — the
// engine avoids Capability Int64, cf. gfx_filter.h FilterPush comments).
// DXC 2018-compatible: no static_assert at global scope.

#ifndef PLACEMENT_COMMON_HLSLI
#define PLACEMENT_COMMON_HLSLI

// Must match C++ RandomChannel.
#define PLACEMENT_CH_POSITION 0
#define PLACEMENT_CH_ROTATION 1
#define PLACEMENT_CH_SCALE    2
#define PLACEMENT_CH_VARIANT  3
#define PLACEMENT_CH_DENSITY  4

uint PlacementSplitMix32(uint x) {
    x += 0x9e3779b9;
    x = (x ^ (x >> 16)) * 0x21f0aaad;
    x = (x ^ (x >> 15)) * 0x735a2d97;
    return x ^ (x >> 15);
}

uint PlacementHashMix32(uint state, uint lane) {
    state ^= PlacementSplitMix32(lane + 0x9e3779b9);
    state *= 0x01000193;
    return PlacementSplitMix32(state);
}

// StableID as uint4 (mirrors C++ StableID::w).
uint4 PlacementMakeStableID(uint seedLo, uint seedHi, uint nodeId, int3 tile,
    uint primitiveId, uint candidateIndex) {
    uint tx = (uint)tile.x, ty = (uint)tile.y, tz = (uint)tile.z;

    uint w0 = seedLo ^ 0x243f6a88;
    w0 = PlacementHashMix32(w0, nodeId);
    w0 = PlacementHashMix32(w0, tx);
    w0 = PlacementHashMix32(w0, ty);
    w0 = PlacementHashMix32(w0, tz);
    w0 = PlacementHashMix32(w0, primitiveId);
    w0 = PlacementHashMix32(w0, candidateIndex);

    uint w1 = seedHi ^ 0x452821e6;
    w1 = PlacementHashMix32(w1, candidateIndex);
    w1 = PlacementHashMix32(w1, primitiveId);
    w1 = PlacementHashMix32(w1, tz);
    w1 = PlacementHashMix32(w1, ty);
    w1 = PlacementHashMix32(w1, tx);
    w1 = PlacementHashMix32(w1, nodeId);

    uint w2 = (seedLo ^ seedHi) ^ 0x38d01377;
    w2 = PlacementHashMix32(w2, nodeId ^ candidateIndex);
    w2 = PlacementHashMix32(w2, tx ^ (tz << 11));
    w2 = PlacementHashMix32(w2, ty ^ (tx >> 7));
    w2 = PlacementHashMix32(w2, primitiveId ^ (ty << 5));

    uint w3 = (seedLo + seedHi) ^ 0xda879e0b;
    w3 = PlacementHashMix32(w3, primitiveId ^ (tz >> 3));
    w3 = PlacementHashMix32(w3, ty ^ candidateIndex);
    w3 = PlacementHashMix32(w3, tx ^ nodeId);
    w3 = PlacementHashMix32(w3, tz ^ (candidateIndex >> 4));

    return uint4(w0, w1, w2, w3);
}

// 64-bit channel hash as uint2 (lo, hi). Mirrors C++ HashChannel.
uint2 PlacementHashChannel(uint4 id, uint channel) {
    uint lo = id.x ^ PlacementSplitMix32(id.y + 0x9e3779b9);
    lo = PlacementHashMix32(lo, id.z);
    lo = PlacementHashMix32(lo, channel);
    uint hi = id.w ^ PlacementSplitMix32(id.z + 0x85ebca6b);
    hi = PlacementHashMix32(hi, id.y);
    hi = PlacementHashMix32(hi, channel ^ 0x9e3779b9);
    return uint2(lo, hi);
}

// [0,1): top 24 bits over 2^24 — exact in f32 on CPU and GPU.
float PlacementRandom01(uint4 id, uint channel) {
    uint2 h = PlacementHashChannel(id, channel);
    return (float)(h.x >> 8) * (1.0f / 16777216.0f);
}

// Two uncorrelated [0,1) values from one channel hash.
float2 PlacementRandom02(uint4 id, uint channel) {
    uint2 h = PlacementHashChannel(id, channel);
    return float2((float)(h.x >> 8) * (1.0f / 16777216.0f),
                  (float)(h.y >> 8) * (1.0f / 16777216.0f));
}

// Winner priority compare: returns true when a beats b (hi, then lo).
bool PlacementPriorityBeats(uint2 a, uint2 b) {
    return (a.y > b.y) || (a.y == b.y && a.x > b.x);
}

// Must match C++ RenderInstance (40 bytes): pos(12) yaw(4) scale(12)
// proto(4) stableLow(4) customB(4).
struct PlacementRenderInstance {
    float3 position;
    float yaw;
    float3 scale;
    uint prototypeId;
    uint stableLow;
    uint customB;
};

// Per-tile output: instance array (fixed bound, BDA view allocates
// nothing) + single-uint append counter.
struct PlacementInstanceBuffer
{
    PlacementRenderInstance items[65536];
};

struct PlacementCountBuffer
{
    uint count;
};

#endif
