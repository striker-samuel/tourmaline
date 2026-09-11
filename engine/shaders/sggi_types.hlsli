#pragma once
// SG-GI probe volume GPU types. Mirrors C++ smvk::gi (Engine/GI/sg.h +
// Engine/GI/probe_volume.h, layout static_asserted there):
//   SgProbeGpu  <-> smvk::gi::SgProbeGpu        (96 B)
//   SgGiLevel   <-> smvk::gi::SgGiLevelGpu      (64 B)
//   SgGiGrid    <-> smvk::gi::SgGiGridGpu       (544 B)
// Included from common.hlsli (before FrameUniforms) so FrameUniforms can
// carry typed pointers into the volume buffers; the sampling/evaluation
// helpers live in sggi.hlsli (included by consumers after common.hlsli).

#define SG_GI_MAX_LEVELS 8

// Fixed SG basis: MUST match smvk::gi::kSgBasis / kSgSharpness (Engine/GI/sg.h).
#define SG_GI_LOBES 9
static const float SG_GI_SHARPNESS = 4.0;
static const float3 SG_GI_AXES[SG_GI_LOBES] = {
    float3( 0.45812285f,  0.88888889f,  0.00000000f),
    float3(-0.54960231f,  0.66666667f,  0.50348074f),
    float3( 0.07831653f,  0.44444444f, -0.89237641f),
    float3( 0.59322548f,  0.22222222f,  0.77375759f),
    float3(-0.98471349f,  0.00000000f, -0.17418195f),
    float3( 0.82265807f, -0.22222222f, -0.52330773f),
    float3(-0.23255520f, -0.44444444f,  0.86509376f),
    float3(-0.34353981f, -0.66666667f, -0.66146500f),
    float3( 0.43032455f, -0.88888889f,  0.15715383f)
};
// Cosine-lobe SG fit (diffuse product integral), mirrors kSgCosine*.
// Clamped-cosine kernel fit: K(t) = int lobe(mu) (w.n)+ dw for
// t = dot(mu, n) is a fixed 1D function of the lobe/normal angle (fixed
// sharpness!), numerically integrated and fitted as a degree-6 Chebyshev
// polynomial (worst 0.33% relative error). MUST match smvk::gi::kSgCosCheb
// (Engine/GI/sg.h).
#define SG_GI_CHEB_TERMS 7
static const float SG_GI_COS_CHEB[SG_GI_CHEB_TERMS] = {
     0.4445942889f,
     0.5893779824f,
     0.1573046510f,
     0.0000000000f,
    -0.0062341036f,
     0.0000000000f,
     0.0002682013f
};

// Probe payload: 96 B. amp = 27 fp16 amplitudes (lobe-major RGB half
// stream, word h>>1 / half h&1), vis = 6 fp16 obstacle distances
// (+x,-x,+y,-y,+z,-z), flags bit0 = valid, bits 8..15 = level.
struct SgProbeGpu
{
    float4 posRadius; // xyz position, w support radius
    uint4 amp[4];
    uint4 vis;
};

// Per-level uniform lookup grid (see probe_volume.h): cellSize >= probe
// support diameter, so a probe sphere intersects <= 8 cells and a cell's
// list is a superset of everything that can shade a point inside it.
struct SgGiLevel
{
    float4 cellSize;   // x = cell size (m)
    float4 volumeMin;  // xyz = level-local grid origin
    uint4 dims;        // cell counts per axis
    uint4 offsets;     // x cells, y indices, z probe base, w pad
};

struct SgGiGrid
{
    float4 volumeMin_probeCount; // xyz global bounds min, w probe count
    uint4 misc;                  // x levelCount, y maxProbesPerCell, zw pad
    SgGiLevel levels[SG_GI_MAX_LEVELS];
};

// Buffer views (DXC needs sized members; mirrors the bake caps).
#define SG_GI_MAX_PROBES 1048576
#define SG_GI_MAX_CELLS 8388608
#define SG_GI_MAX_INDEX_ENTRIES 8388608
struct SgGiProbesBuffer { SgProbeGpu probes[SG_GI_MAX_PROBES]; };
struct SgGiCellsBuffer  { uint2 cells[SG_GI_MAX_CELLS]; };
struct SgGiIndexBuffer  { uint index[SG_GI_MAX_INDEX_ENTRIES]; };
