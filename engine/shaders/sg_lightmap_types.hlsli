#pragma once
// SG lightmap GPU types (included from common.hlsli before FrameUniforms).
// Mirrors smvk::gi (Engine/GI/lightmap.h): the chunk descriptor the
// shader consumes lives here; the sampling/evaluation helpers live in
// sg_lightmap.hlsli (consumers include it after common.hlsli).

// Chunk descriptor (32 B): rect transform for chunk-local uv -> atlas uv.
struct SgLmChunkGpu
{
    float4 rectMinSize; // xy atlas texel min of the rect, zw size in texels
    float4 atlasInfo;   // xy 1/atlasSize, z lobeCount, w pad
};

// Buffer view (DXC needs sized members).
#define SG_LM_MAX_CHUNKS 4096
struct SgLmChunkBuffer { SgLmChunkGpu chunks[SG_LM_MAX_CHUNKS]; };

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
