// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// CPU-only frame-pass policy. This retains the public WorldRendererConfig
// type while keeping pass-selection presets independent of Vulkan/SDL state.
namespace engine {

// ---------------------------------------------------------------------------
// Configurable pass set. RenderFrame consults these flags, so scene samples
// (or A/B experiments) toggle passes without touching pass code. Defaults
// are the full pipeline (previous behavior).
// ---------------------------------------------------------------------------
struct WorldRendererConfig {
    bool directionalShadows = true; // CSM cascade passes
    bool spotShadows = true;        // spot atlas selection + tile passes
    bool forwardPlusPrepass = true; // depth prepass feeding the tile cull
    bool forwardPlusCull = true;    // tile light cull
    bool gfxFilter = true;          // GPU-driven per-view triangle filter
    bool gfxMesh = true;            // task-less mesh-shader main leg (needs device mesh shaders)
    bool filteredMotion = true;     // filtered motion-vector path
    bool motionVectors = true;      // motion-vector target pass
    bool motionBlur = true;         // HDR velocity-gather blur
    bool dof = true;                // depth-of-field (low single-pass / HQ bokeh)
    bool ssr = true;                // screen-space reflections (depth-reconstructed)
    bool volumetric = true;         // raymarched GI-probe volumetric fog (HDR composite)
    bool contactShadows =
        true; // sun contact shadows (pre-Main depth raymarch, min'd with the CSM in the lit body)
    bool ssao = true;            // AO master switch (SSAO/GTAO/SAO per AoMode)
    bool ssgiDiffuse = true;     // diffuse screen-space GI master switch (the runtime
                                 // enable is m_ssgiDiffuseEnabled — MSAA scenes gate
                                 // off at the frame site, no stable 1x G-buffer)
    bool bloom = true;           // HDR bloom chain
    bool exposure = true;        // histogram meter + CPU eye adaptation
    bool tonemap = true;         // HDR -> swapchain tonemap
    bool ui = true;              // separated ImGui target + composite
    bool msaa = true;            // 2x MSAA Main (when device supports it)
    bool skydome = true;         // fullscreen-triangle sky in Main
    bool terrain = false;        // tiled heightfield dressing in Main (off by default)
    bool smaa = true;            // SMAA post-process chain (mode via UI AA combo)
    bool deferredShadows = true; // tiled/zBin deferred-shadow pipeline (H toggles at runtime)
    bool particles = true;       // GPU particle system (compute emitters + tiled-lit billboards)

    static WorldRendererConfig All() {
        return WorldRendererConfig{};
    }
    static WorldRendererConfig Minimal() {
        WorldRendererConfig c{};
        c.directionalShadows = false;
        c.spotShadows = false;
        c.forwardPlusPrepass = false;
        c.forwardPlusCull = false;
        c.gfxFilter = false;
        c.filteredMotion = false;
        c.gfxMesh = false;
        c.motionVectors = false;
        c.motionBlur = false;
        c.dof = false;
        c.ssr = false;
        c.volumetric = false;
        c.contactShadows = false;
        c.ssao = false;
        c.ssgiDiffuse = false;
        c.bloom = false;
        c.exposure = false;
        c.ui = false;
        c.msaa = false;
        c.skydome = false;
        c.terrain = false;
        c.smaa = false;
        c.deferredShadows = false;
        c.particles = false;
        return c;
    }

    // Bisection config: filtered Main + HZB + tonemap only (depth prepass
    // feeds the occluder/HZB chain, the GPU filter picks draws, Main shades
    // survivors). Everything else off.
    static WorldRendererConfig FilterOnly() {
        WorldRendererConfig c = Minimal();
        c.forwardPlusPrepass = true;
        c.gfxFilter = true;
        return c;
    }
};

} // namespace engine
