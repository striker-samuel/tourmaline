// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SG lightmap GPU types (included from common.hlsli before FrameUniforms).
// The 2D SG lightmap atlas: two GPU layouts selected by FrameUniforms.
// lmLayerKind — 0: 9 RGBA16F stream layers (half h = lobe*3 + channel ->
// layer h/4); 1: BC6H one-texture-per-lobe (sglm v3 cook payload).
// There is NO chunk descriptor buffer: each vertex carries its CHART-LOCAL
// uv (TexCoord1) + chart index, each draw carries its mesh's base offset
// into the scene-wide chart transform buffer (GfxDraw.misc2.x), and the
// vertex shader resolves the atlas uv as chartUv * xform.scale +
// xform.offset (see gfxfilter.hlsli GfxLightmapUv). The lobe count rides
// the gfx draw ring (GfxDraw.misc.w = 5/6/9/12, 0xFFFFFFFF = none). The
// fragment shader samples the atlas directly at IN.lightmapUv.

#define SG_LM_MAX_LOBES 12
#define SG_LM_LAYERS 9

// Regular probe-volume (dynamic-object grids) GPU types. The OBB struct
// mirrors smvk::gi::SgRegularObb (Engine/GI/regular_volume.h) with
// explicit float4 padding (std430 block layout: vec3 members would
// 16-byte-stride and desync from the C++ packer). Sampling helpers live
// in sg_regular.hlsli.
#define SG_REG_MAX_VOLUMES 8
#define SG_REG_LAYERS 7 // RGBA16F 3D textures per volume

struct SgRegularObbGpu
{
    float4 center;      // xyz box center, w pad
    float4 axisX;       // xyz orthonormal orientation columns, w pad
    float4 axisY;
    float4 axisZ;
    float4 halfExtents; // xyz half sizes (m), w pad
    uint4 dims;         // xyz lattice resolution, w pad
};

struct SgRegularObbBuffer { SgRegularObbGpu obbs[SG_REG_MAX_VOLUMES]; };

// GI volume (cubemap_volume.h) GPU types. Sampling helpers live in
// sg_gi_volume.hlsli. Header + plane POOL split (the old layout inlined the
// whole 16-plane array in a 560 B aggregate; copying it by value through
// the blend helpers dominated the shader's data-movement instruction
// mix). Volumes are 80 B headers (fade-expanded AABB broad-phase + the
// CPU-precomputed world->uvw lattice transform + the plane span); planes
// live in a shared packed pool fetched one 16 B slot at a time (only
// the planes a volume actually has). All members are float4/uint4 —
// std430 keeps vec3 16-byte-stride, explicit vec4s mirror the C++ packer
// with zero padding surprises (the SgRegularObbGpu rule above).
#define SG_GI_VOLUME_MAX 8
#define SG_GI_VOLUME_LAYERS 7     // RGBA16F 3D textures per volume (sgrv layout)
#define SG_GI_VOLUME_MAX_PLANES 16
// Ray-space table cap for the fog march (sg_gi_volume.hlsli): at most this
// many interval-overlapping volumes hold base/slope coefficients across
// the march; the excess (pathological-only) evaluates per segment from
// the pool, exactly.
#define SG_GI_VOLUME_MAX_RAY 4

// One convex half-space, folded into ONE pre-normalized float4:
//   t = saturate(inside * fadeScale + fadeBias)
//     = saturate(dot(-n * fadeScale, pos) + (fadeBias - d * fadeScale))
//   weightEquation.xyz = -normal * fadeScale
//   weightEquation.w   = fadeBias - d * fadeScale
// (fadeScale = 1/max(insideFade+outsideFade, 1e-5), fadeBias =
// outsideFade * fadeScale — packed by the CPU, LoadGiVolumes). The plane
// test is one dot3 + FMA + saturate on 16 B of data; the second float4
// the old layout carried is gone.
struct GiVolumePlaneGpu
{
    float4 weightEquation; // pre-normalized plane ramp (see above)
};

struct GiVolumeGpu
{
    float4 aabbMin;        // xyz fade-expanded hull AABB min (broad-phase reject)
    float4 aabbMax;        // xyz fade-expanded max, w pad
    float4 uvwScale;       // xyz world->lattice-uvw scale = 1/(spacing*max(dims,1)), w pad
    float4 uvwBias;        // xyz = -gridMin * uvwScale, w pad (uvw = saturate(pos*scale + bias))
    uint4 planeCountBase;  // x plane count, y first plane in the shared pool below, zw pad
};

// One BDA covers headers + the packed plane pool (the volume count rides
// FrameUniforms::giVolumeCount; priority is the UPLOAD order — the app sorts
// low first, the compositor walks high first).
struct GiVolumesBuffer
{
    GiVolumeGpu volumes[SG_GI_VOLUME_MAX];                    // 8 x 80 B
    GiVolumePlaneGpu planes[SG_GI_VOLUME_MAX * SG_GI_VOLUME_MAX_PLANES];   // up to 128 x 16 B
};
