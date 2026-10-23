# Tourmaline — Reusable Copy

## One‑paragraph overview
Tourmaline is a personal C++20 Vulkan 1.3 renderer built from scratch, featuring a custom RHI, GPU‑driven culling, a render‑graph with automatic barrier discipline, forward‑plus shading, SSR, SSAO/SAO, volumetric fog, physically‑based lighting, a content pipeline (glTF/KTX2 → cook → pak), and an ImGui‑based editor with SQLite asset registry. It ships with 35 plain‑exe test executables, 112 HLSL shaders compiled to SPIR‑V at build time, and ~139 k lines of engine source across 350 files.

## Key features (copy‑ready bullets)
- **Custom Vulkan 1.3 RHI** — bindless descriptor pools, automatic barrier discipline via render graph, staging uploads, timeline semaphores.
- **GPU‑driven rendering** — HZB occlusion culling, GPU mesh filter with LOD chains, indexed draws skipping eligible mesh ranges (exactly‑once discipline).
- **Shadows** — cascaded sun + per‑spot shadow atlases, EVSM soft shadows, deferred shadow maps (DPS) with debug visualizers.
- **Lighting & GI** — analytic lights, SSR, raymarched volumetric fog with fog‑GI sampling, SSAO/SAO, lightmap baking (xatlas + Embree), cubemap GI bakes cooked offline into content packs, probe‑premix (blended‑cubemap + dominant‑box parallax vs per‑pixel walk).
- **Post‑processing** — auto exposure, bloom, GT7‑style tonemap, DoF, motion blur, SMAA (1×/T2x), optional DLSS‑SR / Reflex via NVIDIA Streamline interposer.
- **Content pipeline** — loose glTF/KTX2 → gltf_import → cook → pak_cooker (or GiBaker/CubemapBaker) → *.pak archives (LZ4, magic‑versioned) → App.exe/Editor.exe VFS + artifact DB.
- **Editor** — ImGui‑based world editor with gizmos, asset registry (SQLite), fiber job backend, command system with recordable tours, Optick CPU/GPU instrumentation.
- **Headless benches** — deterministic capture via TOURMALINE_CAPTURE + --bench, golden‑image regression with tools/golden_capture.py, A/B matrix of features via env pins.
- **Strict‑resource contract** — call sites decide policy; default graceful fallback, --strict / TOURMALINE_STRICT=1 aborts loudly at exhaustion.

## Technical stack summary
- **Language / standard** — C++20, MSVC 2022+, CMake + Ninja (out‑of‑source only)
- **API** — Vulkan 1.3 (custom RHI, bindless, pipeline caches)
- **Shaders** — 112 HLSL files → SPIR‑V via DXC (`-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout`)
- **Build products** — shaders in out/build/<config>/content/shaders/*.spv; App.exe + Editor.exe in out/build/<config>/
- **Third‑party** — pinned submodules (GLM, nlohmann/json, meshopt, DirectXTex, volk, SPIRV‑Reflect, nanosvg); vendored (Embree, SDL3, ImGui, Optick, SQLiteCpp, xatlas, lz4, Streamline, VMA)
- **Target platform** — Windows 10/11 x64

## Performance claim (with disclaimer)
Local benchmarks on NVIDIA GeForce RTX 5070 Ti, driver 32.0.16.1742, 1920×1080:
- Sponza (SSR + SAO): ~3.2 ms avg frame (hero camera)
- Sponza (different camera): ~10.2 ms
- Asteroids (same camera): ~19.5 ms
- Knight (bloom off): ~6.7 ms
- Per‑feature A/B matrix available in tools/opt_matrix.txt

> **Note** — Frame times vary with camera, scene content, and driver version. Reproduce with `--bench N` + `TOURMALINE_*` env vars.

## AI disclosure blurb
Development of Tourmaline was assisted by AI coding agents: **Muse Spark 1.3** and **GLM 5.3** (Z.ai, driven through the opencode CLI). They contributed to implementation, debugging, testing and documentation across the codebase; the architecture, review and final decisions are the author's.

## License blurb
Copyright (c) 2026 Samuel Martin. Released under the **MIT License**. Third‑party components keep their own licenses (see THIRD_PARTY_NOTICES.md).