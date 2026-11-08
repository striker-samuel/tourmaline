// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Optional DoubleFloat transport helpers for a future GPU scene.
// Mirror: Engine/Core/double_float.h. This header is NOT wired into the
// existing GfxDraw/model or culling shaders; no renderer behavior changes.
//
// high/low encode an absolute double-precision world position as two
// float32 terms. 2x float4 guarantees a simple 32-byte CPU/GPU stride.
// .w is unused, zero-filled padding (not part of the position).
// GPU_STRUCT_NAME(DoubleFloat3)
struct DoubleFloat3 {
    float4 high;
    float4 low;
};

// The origin MUST be split with the same CPU helper as the world position.
// Do not first add high+low in world space, or the low component disappears.
// Use the returned float3 for both instance/meshlet culling and rasterization.
// This is an accurate NEAR-VIEW conversion, not general-purpose FP64 math.
float3 DoubleFloatRelative(DoubleFloat3 world, DoubleFloat3 origin) {
    precise float3 hi = world.high.xyz - origin.high.xyz;
    precise float3 lo = world.low.xyz - origin.low.xyz;
    return hi + lo;
}

// Affine local point -> render-relative position. The 3x3 basis contains
// rotation/scale ONLY; world translation is encoded separately above.
// Keep view/projection matrices in the same render-relative coordinate frame.
float3 DoubleFloatTransformPoint(
    float3 localPoint,
    float3x3 localBasis,
    DoubleFloat3 worldTranslation,
    DoubleFloat3 origin) {
    return mul(localBasis, localPoint)
         + DoubleFloatRelative(worldTranslation, origin);
}
