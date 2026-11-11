// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU-Scene-native flat debug FS: outputs the per-meshlet debug color carried
// in the color varying (location 0), no lighting/textures. Pairs with the
// task/mesh shader (gpu_scene_meshlet.mesh.hlsl) and the indexed raster VS
// (gpu_scene_raster.vert.hlsl) when a flat debug view (Meshlets F3) is
// active, so the view renders the ACTUAL GPU Scene cull survivors instead of
// bypassing the chain (the direct meshdebug.vert/.frag path is the legacy
// "draw every instance" twin). Reads only location 0; the producer still
// writes locations 0..10 (the lit FS interface) so a single VS links both.
// The color arrives pre-scaled by 1/kDebugViewExposure (see the mesh shader),
// so the HDR + ACES chain reproduces the authored display color.

struct PSIn {
    [[vk::location(0)]] float4 color : COLOR0;
};

struct PSOut {
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    OUT.outColor = IN.color;
    return OUT;
}
