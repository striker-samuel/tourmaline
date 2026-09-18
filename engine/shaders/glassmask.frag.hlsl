#include "common.hlsli"

// Glass-layer writer (SSR pane reflections, option B). Pairs with the
// same default.vert VS as the lit legs — a FS consuming only
// SV_Position links with it, same trick as default_wire.frag — so no
// VS twin is needed.
//
// The app's GlassMask pass redraws the transparent list (AlphaMode::
// Blend + dithered screen-door panes — the materials the opaque-only
// prepass depth never carries) into the SSR mask target:
//   R = pane presence (1 wherever a transparent fragment lands),
//   G = the pane fragment's reverse-Z depth — SV_Position.z, the exact
//       value the pane would have written to the depth buffer, so the
//       SSR trace unprojects / view-distances it with the same
//       DofUnprojectView / DofViewDist helpers it uses on depth taps.
//   B/A stay 0/1: under the mask's receiver-material semantics (see
//       ssrmat.frag.hlsl, the OPAQUE redraw of the same pass) panes are
//       non-metal full-gloss receivers — their reflection strength is
//       the trace's dielectric-F0 Fresnel alone.
// The pass blends Max: presence ORs together and depth keeps the
// NEAREST pane where panes stack (reverse-Z max), so draw order never
// matters. Depth test is GreaterEqual against the opaque prepass depth
// (occluded panes must not mask), never writes it.
// No material state: coverage is geometric — alpha/transmission does
// not gate it, a near-clear pane still reflects (Fresnel in the trace
// owns the strength).

struct PSIn
{
    float4 clipPos : SV_Position;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    OUT.outColor = float4(1.0f, IN.clipPos.z, 0.0f, 1.0f);
    return OUT;
}
