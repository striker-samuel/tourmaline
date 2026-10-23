# Portfolio Assets

This folder contains the images used in the Tourmaline portfolio (portfolio.html / portfolio.pdf). Each entry lists the filename, a brief description, and the capture command / environment variables used to produce it.

| File | Description | Capture command |
|---|---|---|
| `hero-sponza-hall.png` | Sponza interior corridor, SMAA, SSR, SAO, 1920×1080. Hero shot for the portfolio. | `TOURMALINE_SSR=1 TOURMALINE_AA=smaat2x TOURMALINE_AUTO_EXPOSURE=0 TOURMALINE_AO=sao TOURMALINE_CAM_POS=2.57,1.07,0.96 TOURMALINE_CAM_FWD=-0.952,0.006,-0.305 TOURMALINE_CAPTURE=36 & .\App.exe --pak content/paks/sponza.pak --disable_control --bench 48` |
| `sponza-courtyard.png` | Sponza courtyard, default sun, clusters. | `TOURMALINE_SSR=1 TOURMALINE_AA=smaat2x TOURMALINE_AUTO_EXPOSURE=0 TOURMALINE_AO=off TOURMALINE_CAM_POS=24.26,1.82,-9.52 TOURMALINE_CAM_FWD=-0.947,-0.022,-0.321 TOURMALINE_CAPTURE=48 & .\App.exe --pak content/paks/sponza.pak --disable_control --bench 64` |
| `gpu-meshlets.png` | GPU meshlet clusters false‑colored over Sponza interior, debug HUD visible. From `docs/images/meshlets_showing.png`. | Original: copied from `docs/images/meshlets_showing.png` (no env vars; static capture). |
| `debug-tools.png` | Debug menus and VRAM usage breakdown, Tourmaline Debug panel. From `docs/images/debug_menus.png`. | Original: copied from `docs/images/debug_menus.png`. |
| `gi-probe-ball.png` | GI probe ball (convex‑volume + octree bake), baked ambient. From `out/logs/gi_final.png`. | Original: copied from `out/logs/gi_final.png` (no env vars; golden capture). |
| `knight-viz.png` | Knight skeletal animation test, manual exposure, bloom off. | `TOURMALINE_AA=smaat2x TOURMALINE_AUTO_EXPOSURE=0 TOURMALINE_BLOOM_INTENSITY=0 TOURMALINE_BLOOM_THRESHOLD=16000 TOURMALINE_CAPTURE=24 TOURMALINE_CAM_POS=2.6,1.55,3.6 TOURMALINE_CAM_FWD=-0.57,-0.12,-0.81 & .\App.exe --pak content/paks/knight.pak --disable_control --bench 32` |

---
*All captures at 1920×1080 (DEFAULT_WIDTH/HEIGHT). Environment follows AGENTS.md: `TOURMALINE_SSR=1`, `TOURMALINE_AA=off`, `TOURMALINE_AUTO_EXPOSURE=0` as defaults; overridden per‑shot as noted.*