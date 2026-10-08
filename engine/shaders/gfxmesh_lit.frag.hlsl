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
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    float2 lightmapUv : TEXCOORD3;
    nointerpolation uint lmChunk : TEXCOORD4;
    // Mesh-only: per-draw material (misc2.y) for the multi-material dispatch.
    nointerpolation uint meshMaterial : TEXCOORD5;
    bool isFrontFace : SV_IsFrontFace;
};
#define GFX_MATERIAL_ID (IN.meshMaterial)
#include "default_lit_body.hlsli"
