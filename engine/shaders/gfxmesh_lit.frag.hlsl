// Mesh-path twin of default_lit.frag.hlsl (classic alpha-test cutout).
//
// Same body (default_lit_body.hlsli), but the material comes from a flat
// mesh-shader varying (location 7) instead of the per-range
// pc.materialId push — one DrawMeshTasks dispatch covers many materials.
// Varyings 0..6 match the indexed VSOut exactly; only location 7 is new,
// so this links ONLY against the mesh shader (gfxfilter.mesh.hlsl), never
// the classic VS (which would leave location 7 unwritten).
//
// C++ side: GfxRenderer mesh twin program { mesh, this FS }.

#define GFXMESH_FS_PASS
#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
// #include "sg_lightmap.hlsli" // Lightmaps deprecated
#include "sg_regular.hlsli"
#include "sg_cvol.hlsli"

#define GFX_MESH_PSIN
struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    // Mesh-only: per-draw material (misc2.y) for the multi-material dispatch.
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
    // Dither-on-closeup flag (flat; Scene::ditherCloseup — the ring's
    // GFX_DRAW_DITHER_CLOSEUP forwarded by the mesh MS; see the body's
    // discard + common.hlsli CloseupDitherKeep). EXPLICIT location 8
    // (pinned like every ditherCloseup declaration — the mesh MS writes
    // location 8; sequential assignment happened to match here but
    // dead-stripping could shift it).
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    bool isFrontFace : SV_IsFrontFace;
};
#define GFX_MATERIAL_ID (IN.meshMaterial)
#include "default_lit_body.hlsli"
