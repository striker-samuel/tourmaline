// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_field.hlsli — GPU mirror of placement_ir.h + placement_eval.h.
// THE program evaluator (PR02): the kernels run the actual graph SSA —
// PlacementEvaluateField implements every FieldOp, mirroring the CPU
// EvaluateProgram instruction-for-instruction (same degenerate-case
// handling, same numeric safeguards; last-ULP float divergence is
// absorbed by PlacementQuantizeDensity before every accept decision).
// 32-bit integer math only. DXC-2018 compatible.

#include "gpu_platform.hlsli"

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
#define PLACEMENT_OP_CLAMP01 10
#define PLACEMENT_OP_REMAP    11
#define PLACEMENT_OP_SMOOTHSTEP 12
#define PLACEMENT_OP_POWER    13
#define PLACEMENT_OP_INVERT   14
#define PLACEMENT_OP_SLOPEMASK 15
#define PLACEMENT_OP_ALTIMASK  16
#define PLACEMENT_OP_ECOMASK   17  // painted ecotope weight, params[0] = channel

// Must match C++ kPlacementMaxFieldInstrs.
#define PLACEMENT_MAX_FIELD_INSTRS 64

// Must match C++ GpuFieldInstr (32 bytes: uint4 + float4, 16-aligned).
struct PlacementFieldInstr
{
    uint op;
    uint a;
    uint b;
    uint res;
    float4 params;
};

struct PlacementInstrBuffer
{
    PlacementFieldInstr i[PLACEMENT_MAX_FIELD_INSTRS];
};

// The ecotope weight mask (PR04): a channel-planar float grid over
// world XZ. Must match C++ EcotopeMaskData's layout
// ([c][z][x], channels*height*width floats) + the same texel mapping
// (origin/spacing, the 1.001 edge clamp) and bilinear composition —
// painting density must never reshuffle unrelated stable IDs.
struct PlacementEcoBuffer { float w[1048576]; }; // 4M floats max (16MB)
struct PlacementEcoMask
{
    GPU_PTR_READ(PlacementEcoBuffer) weights; // 0
    uint channels; // 8
    uint w;        // 12
    uint h;        // 16
    uint pad;      // 20
    float ox;      // 24
    float oz;      // 28
    float sx;      // 32
    float sz;      // 36
};

// Bilinear, edge-clamped ecotope weight — mirrors C++
// EcotopeMaskData::Sample exactly (including the defensive 0 on an
// out-of-range channel; validated programs never send one).
float PlacementSampleEcoMask(PlacementEcoMask eco, uint channel,
    float x, float z)
{
    if (channel >= eco.channels)
        return 0.0f;
    float gx = clamp((x - eco.ox) / eco.sx, 0.0f, (float)eco.w - 1.001f);
    float gz = clamp((z - eco.oz) / eco.sz, 0.0f, (float)eco.h - 1.001f);
    uint x0 = (uint)gx, z0 = (uint)gz;
    float fx = gx - (float)x0, fz = gz - (float)z0;
    uint base = (channel * eco.h + z0) * eco.w + x0;
    float w00 = eco.weights.Get().w[base];
    float w10 = eco.weights.Get().w[base + 1];
    float w01 = eco.weights.Get().w[base + eco.w];
    float w11 = eco.weights.Get().w[base + eco.w + 1];
    return w00 * (1 - fx) * (1 - fz) + w10 * fx * (1 - fz)
         + w01 * (1 - fx) * fz + w11 * fx * fz;
}

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

// The CPU Smoothstep mirror INCLUDING the degenerate edge: e1 <= e0 is
// a hard step at e1 (never a division by zero).
float PlacementSmoothstepF(float e0, float e1, float x) {
    if (!(e1 > e0))
        return x >= e1 ? 1.0f : 0.0f;
    float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// THE evaluator: values[k] per instruction, SSA operands read EARLIER
// values. Mirrors C++ EvaluateProgram (placement_eval.cpp) exactly:
//   - slopeDeg derived from n.y with the same >=1 / <=0 guards;
//   - Remap's zero-denominator -> 0 form;
//   - Power's max(a, 0) domain guard;
//   - Noise2 feeds (p.x * p0 + p1, p.z * p0 + p2) through the same
//     lattice-hash value noise with the graph seed.
// Returns values[acceptIndex] (the density BEFORE densityScale and
// quantization — the caller applies those in the contract order:
//   q = QuantizeDensity(d * densityScale);
//   keep = Random01(id, Density) < q).
// ---------------------------------------------------------------------------
float PlacementEvaluateField(
    GPU_PTR(PlacementInstrBuffer) instrs,
    uint instrCount, uint acceptIndex,
    float3 p, float3 n, uint seedLo, uint seedHi,
    PlacementEcoMask eco)
{
    float v[PLACEMENT_MAX_FIELD_INSTRS];
    const float slopeDeg = (n.y >= 1.0f) ? 0.0f
        : (n.y <= 0.0f) ? 90.0f
        : acos(clamp(n.y, -1.0f, 1.0f)) * 57.29578f;
    [loop]
    for (uint k = 0; k < instrCount; ++k)
    {
        PlacementFieldInstr ins = instrs.Get().i[k];
        // Defensive operand reads (identical to the CPU): validated
        // programs always have a/b < k, out-of-range reads 0.
        float av = (ins.a < instrCount) ? v[ins.a] : 0.0f;
        float bv = (ins.b < instrCount) ? v[ins.b] : 0.0f;
        float r = 0.0f;
        switch (ins.op)
        {
        case PLACEMENT_OP_CONST:
            r = ins.params.x;
            break;
        case PLACEMENT_OP_POSX:
            r = p.x;
            break;
        case PLACEMENT_OP_POSY:
            r = p.y;
            break;
        case PLACEMENT_OP_POSZ:
            r = p.z;
            break;
        case PLACEMENT_OP_NORMALY:
            r = n.y;
            break;
        case PLACEMENT_OP_NOISE2:
            r = PlacementValueNoise2D(
                p.x * ins.params.x + ins.params.y,
                p.z * ins.params.x + ins.params.z,
                seedLo, seedHi);
            break;
        case PLACEMENT_OP_ADD:
            r = av + bv;
            break;
        case PLACEMENT_OP_MUL:
            r = av * bv;
            break;
        case PLACEMENT_OP_MIN:
            r = min(av, bv);
            break;
        case PLACEMENT_OP_MAX:
            r = max(av, bv);
            break;
        case PLACEMENT_OP_CLAMP01:
            r = clamp(av, 0.0f, 1.0f);
            break;
        case PLACEMENT_OP_REMAP: {
            float denom = ins.params.y - ins.params.x;
            float t = (denom != 0.0f)
                ? (av - ins.params.x) / denom : 0.0f;
            r = ins.params.z + t * (ins.params.w - ins.params.z);
            break;
        }
        case PLACEMENT_OP_SMOOTHSTEP:
            r = PlacementSmoothstepF(ins.params.x, ins.params.y, av);
            break;
        case PLACEMENT_OP_POWER:
            r = pow(max(av, 0.0f), ins.params.x);
            break;
        case PLACEMENT_OP_INVERT:
            r = 1.0f - av;
            break;
        case PLACEMENT_OP_SLOPEMASK:
            r = 1.0f - PlacementSmoothstepF(ins.params.x, ins.params.y, slopeDeg);
            break;
        case PLACEMENT_OP_ALTIMASK: {
            float lo = PlacementSmoothstepF(ins.params.x, ins.params.y, p.y);
            float hi = 1.0f - PlacementSmoothstepF(ins.params.z, ins.params.w, p.y);
            r = lo * hi;
            break;
        }
        case PLACEMENT_OP_ECOMASK:
            r = PlacementSampleEcoMask(eco, (uint)max(ins.params.x, 0.0f),
                p.x, p.z);
            break;
        default:
            r = 0.0f; // validated programs never reach this
            break;
        }
        v[k] = r;
    }
    return (acceptIndex < instrCount) ? v[acceptIndex] : 0.0f;
}

#endif
