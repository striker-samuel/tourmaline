// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Packed vertex-stream unpack (CPU mirror: Engine/Renderer/vertex_pack.h —
// keep the two in sync: same fold, same sign(0)=+1, same rounding).
//
// VertexShading is 16 bytes: color RGBA8_UNORM, uv RG16_FLOAT (both decoded
// by the vertex-fetch hardware into float), normal/tangent as R32_UInt
// octahedral words decoded here. The bitangent is never stored; the
// fragment shader rebuilds B = cross(N, T) * w.
#pragma once

float OctSignH(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

float3 OctDecodeNormal(uint packed)
{
    uint ux = packed & 0xFFFFu;
    uint uy = (packed >> 16) & 0xFFFFu;
    float x = (float)ux * (2.0f / 65535.0f) - 1.0f;
    float y = (float)uy * (2.0f / 65535.0f) - 1.0f;
    // z keeps its (negative) pre-fold value: the fold only undoes the
    // hemisphere flip of xy (recomputing z from the folded xy would flip
    // the hemisphere back and break the -Z pole).
    float z = 1.0f - abs(x) - abs(y);
    if (z < 0.0f)
    {
        float tx = (1.0f - abs(y)) * OctSignH(x);
        float ty = (1.0f - abs(x)) * OctSignH(y);
        x = tx;
        y = ty;
    }
    float l2 = x * x + y * y + z * z;
    if (!(l2 > 1e-24f))
        return float3(0.0f, 0.0f, 1.0f);
    return float3(x, y, z) / sqrt(l2);
}

float4 OctDecodeTangent(uint packed)
{
    uint ux = (packed >> 16) & 0xFFFFu;
    uint uy = (packed >> 1) & 0x7FFFu;
    float w = (packed & 1u) ? 1.0f : -1.0f;
    float x = (float)ux * (2.0f / 65535.0f) - 1.0f;
    float y = (float)uy * (2.0f / 32767.0f) - 1.0f;
    // z keeps its (negative) pre-fold value (see OctDecodeNormal).
    float z = 1.0f - abs(x) - abs(y);
    if (z < 0.0f)
    {
        float tx = (1.0f - abs(y)) * OctSignH(x);
        float ty = (1.0f - abs(x)) * OctSignH(y);
        x = tx;
        y = ty;
    }
    float l2 = x * x + y * y + z * z;
    if (!(l2 > 1e-24f))
        return float4(1.0f, 0.0f, 0.0f, w);
    return float4(float3(x, y, z) / sqrt(l2), w);
}
