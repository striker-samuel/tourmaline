// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_field.hlsli — GPU mirror of placement_ir.h + placement_eval.h.
// Per-op building blocks + value noise so compiler-generated straight-line
// field code stays tiny. 32-bit integer math only. DXC-2018 compatible.

#ifndef PLACEMENT_FIELD_HLSLI
#define PLACEMENT_FIELD_HLSLI

// Must match C++ FieldOp numeric values.
#define PLACEMENT_OP_CONST    0
#define PLACEMENT_OP_POSX     1
#define PLACEMENT_OP_POSY     2
#define PLACEMENT_OP_POSZ     3
#define PLACEMENT_OP_NORMALY  4
#define PLACEMENT_OP_NOISE2   5
#define PLACEMENT_OP_ADD      6
#define PLACEMENT_OP_MUL      7
#define PLACEMENT_OP_MIN      8
#define PLACEMENT_OP_MAX      9
#define PLACEMENT_OP_CLAMP01  10
#define PLACEMENT_OP_REMAP    11
#define PLACEMENT_OP_SMOOTHSTEP 12
#define PLACEMENT_OP_POWER    13
#define PLACEMENT_OP_INVERT   14
#define PLACEMENT_OP_SLOPEMASK 15
#define PLACEMENT_OP_ALTIMASK  16

float PlacementValueNoise2D(float x, float z, uint seedLo, uint seedHi) {
    int2 base = int2((int)floor(x), (int)floor(z));
    float2 f = float2(x - (float)base.x, z - (float)base.y);
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    uint s = (seedLo ^ (seedHi + 0x9e3779b9));
    uint h00 = PlacementHashMix32(PlacementHashMix32(s, (uint)base.x), (uint)base.y);
    uint h10 = PlacementHashMix32(PlacementHashMix32(s, (uint)(base.x + 1)), (uint)base.y);
    uint h01 = PlacementHashMix32(PlacementHashMix32(s, (uint)base.x), (uint)(base.y + 1));
    uint h11 = PlacementHashMix32(PlacementHashMix32(s, (uint)(base.x + 1)), (uint)(base.y + 1));
    float v00 = (float)(h00 >> 8) * (1.0f / 16777216.0f);
    float v10 = (float)(h10 >> 8) * (1.0f / 16777216.0f);
    float v01 = (float)(h01 >> 8) * (1.0f / 16777216.0f);
    float v11 = (float)(h11 >> 8) * (1.0f / 16777216.0f);
    return lerp(lerp(v00, v10, u.x), lerp(v01, v11, u.x), u.y);
}

// 8-bit density quantization before threshold (strong determinism).
float PlacementQuantizeDensity(float d) {
    if (d <= 0.0f) return 0.0f;
    if (d >= 1.0f) return 1.0f;
    return floor(d * 255.0f) * (1.0f / 255.0f);
}

#endif
