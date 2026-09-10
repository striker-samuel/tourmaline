#pragma once
// RenderView – a virtual camera + its associated render target.
//
// Mirrors C++ Engine/Renderer/renderview.h byte-for-byte (256 bytes,
// compiled with -fvk-use-dx-layout). A RenderView is the single unit of
// "something that can render": the main game camera allocates 1 view,
// each directional light allocates 1 view per shadow cascade (4 for the
// standard CSM setup), shadowed spots / custom passes allocate their own.
//
// The per-frame slab is reachable through PushConstants::renderViews;
// passes pick their camera with pc.renderViewIndex / pc.mainRenderView.

// C++ side expands this to alignas(16); HLSL structs follow the DX layout
// rules, so here it is only a parity marker.
#define HLSL_ALIGN_STRUCT

#define MAX_RENDER_VIEWS 64

// What a view is used for (mirrors C++ RenderViewFlagBits).
#define RENDERVIEW_FLAG_NONE           (0u)
#define RENDERVIEW_FLAG_MAIN_VIEW      (1u << 0)
#define RENDERVIEW_FLAG_SHADOW_CASCADE (1u << 1)
#define RENDERVIEW_FLAG_SPOT_SHADOW    (1u << 2)
#define RENDERVIEW_FLAG_CUSTOM         (1u << 3)

// One virtual camera + its render target (256 bytes).
// m_viewPlanes: inward frustum planes (xyz normal, w distance), normalized.
// m_renderTargetSize: xy = target size (px), zw = 1/size.
struct HLSL_ALIGN_STRUCT RenderView
{
    float4x4 m_viewProjectionMatrix;
    float4x4 m_inverseViewProjectionMatrix;
    float4 m_viewPlanes[6];
    float4 m_renderTargetSize;
    uint m_renderViewFlags;
    uint3 m_padding0;
};

// Per-frame GPU slab, indexed from the push constants.
struct RenderViewBuffer
{
    RenderView views[MAX_RENDER_VIEWS];
};
