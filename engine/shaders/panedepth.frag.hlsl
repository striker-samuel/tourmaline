#include "common.hlsli"

// Pane-depth writer (SSR pane reflections — see the app's PaneMask pass +
// ssr.h). Pairs with the same default.vert VS as the lit legs — a FS
// consuming only SV_Position links with it, same trick as default_wire.frag
// — so no VS twin is needed. The successor of the old glassmask.frag (the
// pane half of the retired GlassMask redraw): the redraw of the
// transparent list (AlphaMode::Blend + dithered screen-door panes — the
// materials the opaque-only prepass depth never carries) now writes
//   R = the pane fragment's reverse-Z depth — SV_Position.z, the exact
//       value the pane would have written to the depth buffer, so the
//       SSR trace unprojects / view-distances it with the same
//       DofUnprojectView / DofViewDist helpers it uses on depth taps,
//   stencil aspect = STENCIL_SSR (REPLACE with the pass's dynamic
//       reference — see Rendergraph/stencil_bits.h; Main's opaque binds
//       write the classification — this pass only marks panes).
// The pass blends Max on R: the NEAREST pane wins where panes stack
// (reverse-Z max), so draw order never matters. Depth test is GreaterEqual
// against the stored opaque depth (occluded panes must not mask), never
// writes it. No material state: coverage is geometric — alpha/transmission
// does not gate it, a near-clear pane still reflects (Fresnel in the trace
// owns the strength).

struct PSIn
{
    float4 clipPos : SV_Position;
};

struct PSOut
{
    float4 outDepth : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    // R16_UNorm target, Max-blended: (z, 0, 0, 1). Clear 0 = no pane;
    // the trace's DofIsBackgroundOrGarbage check rejects pane taps at 0.
    OUT.outDepth = float4(IN.clipPos.z, 0.0f, 0.0f, 1.0f);
    return OUT;
}
