// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_domains.hlsli — GPU domain sampling for the scatter kernels.
// Mirrors placement_domain.h exactly (alias walk, barycentric reflect,
// bilinear height + central-difference normal). Positions/normals arrive
// as float4 arrays (explicit padding — never float3, whose HLSL array
// stride is layout-dependent). DXC-2018 compatible.

#ifndef PLACEMENT_DOMAINS_HLSLI
#define PLACEMENT_DOMAINS_HLSLI

#define PLACEMENT_MAX_VERTS 65536
#define PLACEMENT_MAX_TRIS  131072
#define PLACEMENT_MAX_HF    1048576

struct PlacementPosBuffer { float4 p[PLACEMENT_MAX_VERTS]; };
struct PlacementNrmBuffer { float4 n[PLACEMENT_MAX_VERTS]; };
struct PlacementUVBuffer { float2 t[PLACEMENT_MAX_VERTS]; };
struct PlacementIndexBuffer { uint i[PLACEMENT_MAX_TRIS * 3]; };
struct PlacementProbBuffer { float p[PLACEMENT_MAX_TRIS]; };
struct PlacementAliasBuffer { uint a[PLACEMENT_MAX_TRIS]; };
struct PlacementHeightBuffer { float h[PLACEMENT_MAX_HF]; };

// Must match C++ PlacementDomainParams (192 bytes, dx-layout).
//
// PR02: the DENSITY itself moved into the uploaded program (the
// instrBDA tail) — the old density/noiseAmp/noiseFreq slots are retired
// (density became densityScale; the noise slots must be 0 — the
// program's Noise2 instructions are the noise source now).
struct PlacementDomainParams
{
    float densityScale; // 0 (was: the const-extracted density shortcut)
    float noiseAmpRetired;  // 4  RETIRED: 0
    float noiseFreqRetired; // 8  RETIRED: 0
    float minScale;   // 12
    float maxScale;   // 16
    uint protoCount;  // 20
    uint padA;        // 24
    float weights0;   // 32
    float weights1;   // 36
    float weights2;   // 40
    float weights3;   // 44
    float weights4;   // 48
    float weights5;   // 52
    float weights6;   // 56
    float weights7;   // 60
    uint domainKind;  // 64
    uint triCount;    // 68
    uint vertCount;   // 72
    uint flags;       // 76 bit0 normals, bit1 uvs
    vk::BufferPointer<PlacementPosBuffer> pos;     // 80
    vk::BufferPointer<PlacementNrmBuffer> nrm;     // 88
    vk::BufferPointer<PlacementUVBuffer> uv;       // 96
    vk::BufferPointer<PlacementIndexBuffer> idx;   // 104
    vk::BufferPointer<PlacementProbBuffer> prob;   // 112
    vk::BufferPointer<PlacementAliasBuffer> alias; // 120
    vk::BufferPointer<PlacementHeightBuffer> heights; // 128
    uint hfW;         // 136
    uint hfH;         // 140
    float hfOX;       // 144
    float hfOZ;       // 148
    float hfSX;       // 152
    float hfSZ;       // 156
    float extMinX;    // 160
    float extMinZ;    // 164
    float extMaxX;    // 168
    float extMaxZ;    // 172
    // ---- the program tail (PR02: the graph SSA) ----
    vk::BufferPointer<PlacementInstrBuffer> instrs; // 176
    uint instrCount;   // 184
    uint acceptIndex;  // 188
    // ---- the ecotope-mask tail (PR04: the painted layer) ----
    vk::BufferPointer<PlacementEcoBuffer> eco; // 192 (0 = none)
    uint ecoW;         // 200
    uint ecoH;         // 204
    uint ecoChannels;  // 208
    uint padEco;       // 212
    float ecoOX;       // 216
    float ecoOZ;       // 220
    float ecoSX;       // 224
    float ecoSZ;       // 228
};

// RETIRED (PR02): PlacementScatterDensity — the old base+noise density
// shortcut. The kernels now call PlacementEvaluateField
// (placement_field.hlsli) with the params' program tail; the density
// order contract is:
//   d  = PlacementEvaluateField(instrs, count, accept, p, n, seedLo, seedHi)
//   q  = PlacementQuantizeDensity(d * prm.densityScale)
//   keep = PlacementRandom01(id, PLACEMENT_CH_DENSITY) < q

uint PlacementScatterProto(float r, PlacementDomainParams prm)
{
    float w0 = prm.weights0, w1 = prm.weights1, w2 = prm.weights2, w3 = prm.weights3;
    float w4 = prm.weights4, w5 = prm.weights5, w6 = prm.weights6, w7 = prm.weights7;
    float total = w0 + w1 + w2 + w3 + w4 + w5 + w6 + w7;
    if (prm.protoCount == 0 || total <= 0.0f)
        return 0;
    float target = clamp(r, 0.0f, 0.99999994f) * total;
    float acc = 0.0f;
    uint count = prm.protoCount;
    acc += w0; if (target < acc) return 0;
    acc += w1; if (target < acc) return min(1, (int)count - 1);
    acc += w2; if (target < acc) return min(2, (int)count - 1);
    acc += w3; if (target < acc) return min(3, (int)count - 1);
    acc += w4; if (target < acc) return min(4, (int)count - 1);
    acc += w5; if (target < acc) return min(5, (int)count - 1);
    acc += w6; if (target < acc) return min(6, (int)count - 1);
    return min(7, (int)count - 1);
}

// Barycentric reflect (mirrors CPU SampleMeshSurface).
float2 PlacementReflectBary(float u, float v)
{
    u = clamp(u, 0.0f, 0.99999994f);
    v = clamp(v, 0.0f, 0.99999994f);
    if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
    return float2(u, v);
}

// Mesh-surface sample. Returns position; normal via out param.
// Mirrors CPU SampleMeshSurface (alias column from r1, keep/fallback
// from r2, placement from bx/by).
float3 PlacementSampleMesh(PlacementDomainParams prm,
    float r1, float r2, float bx, float by, out float3 nrm, out uint tri)
{
    uint n = prm.triCount;
    nrm = float3(0, 1, 0);
    tri = 0;
    if (n == 0)
        return float3(0, 0, 0);
    r1 = clamp(r1, 0.0f, 0.99999994f);
    r2 = clamp(r2, 0.0f, 0.99999994f);
    uint col = (uint)(r1 * (float)n);
    if (col >= n) col = n - 1;
    tri = col;
    if (r2 >= prm.prob.Get().p[col])
        tri = prm.alias.Get().a[col];
    float2 buv = PlacementReflectBary(bx, by);
    float u = buv.x, v = buv.y, w = 1.0f - u - v;
    uint i0 = prm.idx.Get().i[tri * 3 + 0];
    uint i1 = prm.idx.Get().i[tri * 3 + 1];
    uint i2 = prm.idx.Get().i[tri * 3 + 2];
    float3 a = prm.pos.Get().p[i0].xyz;
    float3 b = prm.pos.Get().p[i1].xyz;
    float3 c = prm.pos.Get().p[i2].xyz;
    if ((prm.flags & 1) != 0)
    {
        float3 na = prm.nrm.Get().n[i0].xyz;
        float3 nb = prm.nrm.Get().n[i1].xyz;
        float3 nc = prm.nrm.Get().n[i2].xyz;
        nrm = normalize(na * w + nb * u + nc * v);
    }
    else
    {
        nrm = normalize(cross(b - a, c - a));
    }
    return a * w + b * u + c * v;
}

// Heightfield bilinear sample + central-difference normal.
// Mirrors HeightfieldData::sampleHeight/sampleNormal exactly,
// including the 1.001 edge clamp.
float PlacementHfHeight(PlacementDomainParams prm, float x, float z)
{
    float gx = (x - prm.hfOX) / prm.hfSX;
    float gz = (z - prm.hfOZ) / prm.hfSZ;
    gx = clamp(gx, 0.0f, (float)prm.hfW - 1.001f);
    gz = clamp(gz, 0.0f, (float)prm.hfH - 1.001f);
    uint x0 = (uint)gx, z0 = (uint)gz;
    float fx = gx - (float)x0, fz = gz - (float)z0;
    float h00 = prm.heights.Get().h[z0 * prm.hfW + x0];
    float h10 = prm.heights.Get().h[z0 * prm.hfW + x0 + 1];
    float h01 = prm.heights.Get().h[(z0 + 1) * prm.hfW + x0];
    float h11 = prm.heights.Get().h[(z0 + 1) * prm.hfW + x0 + 1];
    return h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz)
         + h01 * (1 - fx) * fz + h11 * fx * fz;
}

float3 PlacementHfNormal(PlacementDomainParams prm, float x, float z)
{
    float e = min(prm.hfSX, prm.hfSZ);
    float hx1 = PlacementHfHeight(prm, x + e, z);
    float hx0 = PlacementHfHeight(prm, x - e, z);
    float hz1 = PlacementHfHeight(prm, x, z + e);
    float hz0 = PlacementHfHeight(prm, x, z - e);
    return normalize(float3((hx0 - hx1) / (2.0f * e), 1.0f, (hz0 - hz1) / (2.0f * e)));
}

#endif
