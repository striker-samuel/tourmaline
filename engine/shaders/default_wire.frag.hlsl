#include "common.hlsli"

// Wireframe (P) flat line color for PolygonMode::Line draws. Pairs with the
// same VSs as the shaded legs (default.vert / gfxfiltered.vert /
// default_vertexlit.vert): a FS consuming only SV_Position links with any of
// them (Vulkan allows FS inputs to be a subset of VS outputs), so no VS
// twins are needed. No lighting, no textures, no texture-streaming feedback
// on line pixels — the heavy lit FS was the wireframe slowdown; see the
// wireTwinOf swap in scene_renderer.cpp.
//
// Exposure-compensated like the other debug views (default_unlit et al): the
// line color travels through HDR + tonemap (hdr * exposure -> ACES) with a
// physical-units exposure (~0.001) that would crush a raw 0..1 color to
// black, so the output is pre-scaled by 1/exposure.

struct PSIn
{
    float4 clipPos : SV_Position;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    FrameUniforms frame = pc.frameUniforms.Get();
    float exp = max(frame.exposure, 1e-6f);

    // Authored display color; tweak here (amber reads against most scenes).
    const float3 wireColor = float3(0.95f, 0.85f, 0.25f);

    OUT.outColor = float4(SanitizeHdr(wireColor / exp), 1.0f);
    return OUT;
}
