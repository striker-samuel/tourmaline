#pragma once
// SG lightmap GPU types (included from common.hlsli before FrameUniforms).
// The 2D SG lightmap atlas: 9 RGBA16F 2D textures shared by all chunks.
// There is NO chunk descriptor buffer: the cook bakes each chart's
// atlas-space transform into the mesh's texCoord2 (RG16_UNORM vertex
// attribute), and the lobe count rides the gfx draw ring (GfxDraw.misc.w
// = 5/6/9/12, 0xFFFFFFFF = none). The fragment shader samples the atlas
// directly at IN.lightmapUv.

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
