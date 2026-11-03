# RFC: one-command native Tourmaline game starter (renderer-backend agnostic)

**Status:** implementation source committed in [PR #85](https://github.com/striker-samuel/tourmaline/pull/85); full cook/GPU smoke test pending  
**Target:** standalone C++20 game in a separate repository, consuming Tourmaline as a pinned Git submodule  
**Related (avoid duplication):** [editor source hot reload PR #84](https://github.com/striker-samuel/tourmaline/pull/84)

## Implementation included in PR #85

- `templates/native_game/` contains a copyable CMake/game/scene/glTF
  example using `app::RunGame`, the real `WorldRenderer` and
  `GameUiRenderer`.
- `TOURMALINE_ENABLE_GAME_HOST` is an opt-in subproject path for the
  Tourmaline game host, `Project` cooker and shader pack while omitting
  repository-only editor/sample targets.
- `WorldRenderer::SetExtraGameUi` records `GameUiRenderer` into the
  existing UI target after the engine's UI pass and before final composite.
- The template documents how to register Tourmaline as a Git submodule;
  the checked-in `.gitmodules` template is not itself a registered gitlink.
- The authored cube/floor scene, data-URI glTF geometry, PNG texture,
  CMake/recipe wiring, CPU tests and static verifier are included.
- **Not yet verified:** full native engine compile, cook, and live GPU
  rendering in the authoring environment. Review against the checklist.

## Problem observed in a real game prototype

While implementing *The Sun Demands Three* as a small incremental game, the path of least resistance was a dedicated SDL3 2D renderer for textured cube placeholders, bespoke mouse hit testing and a tiny hand-coded font. That is a poor outcome for a game intended to render **through Tourmaline's native engine abstraction**. Tourmaline currently uses Vulkan; a future Metal or Direct3D backend should not force game projects to change their renderer-facing code. SDL3 can remain the window/input dependency under the engine, but should not become a separate game graphics renderer simply because the native bootstrap is hard to discover.

This is not a request for an ECS or a second renderer. Tourmaline **already** provides the underlying systems:

- `app::RunGame`, `app::GameBehaviour` and the project-catalog/runtime gates (see `app/src/Runtime.h` and `samples/04_project_game`).
- Tourmaline's `WorldRenderer` (currently implemented on Vulkan), scene runtime, typed project catalog and cooked `.tour` content.
- `tour::GameUiRenderer` + `UiLayout2D` (already merged in [PR #43](https://github.com/striker-samuel/tourmaline/pull/43)) with anchored rectangles, clipping and hit-test bounds.
- `tour::World2DRenderer` and `.spriteset` assets (PRs [#43](https://github.com/striker-samuel/tourmaline/pull/43) and [#55](https://github.com/striker-samuel/tourmaline/pull/55)).

The gap is a **small, documented, buildable composition of those pieces**, not necessarily new subsystems. `04_project_game` is excellent documentation for the full content-as-code workflow but too feature-rich to function as the smallest first game.

## Requested deliverable (P0)

Add `templates/native_game/` (or one equivalently first-class `samples/06_native_game_starter/`) that a developer can copy to a standalone top-level repo and build with Tourmaline as a **Git submodule**:

```text
MyGame/
  CMakeLists.txt                 # self-contained top-level Ninja configure
  .gitmodules                    # pinned Tourmaline, not a duplicated engine
  src/main.cpp                   # app::RunGame, Tourmaline WorldRenderer
  src/game.cpp                   # unopinionated C++ state and fixed/ticked update
  project/game.tourrecipe        # tiny baked project with one scene
  project/content/scenes/main.tour
  project/content/models/placeholder/  # UV-mapped cube, material/PNG
  project/project.h              # generated catalog, not hand-authored
  README.md                      # build, cook, run and asset replacement steps
  tests/                         # CPU state tests + optional rendering smoke test
  external/tourmaline/           # Git submodule
```

The template must:

1. **Render through Tourmaline's native `WorldRenderer` end to end, independent of graphics API.** Boot with `app::RunGame`/`tourmaline_apphost`; show a textured 3D cube, floor and working camera/light using Tourmaline's renderer-facing APIs and current configured backend (Vulkan today). Do not call Vulkan-specific APIs from game-owned code or hardcode `Vk*` handles, `#include <vulkan/...>`, or Vulkan resource lifetime. A future Metal or Direct3D backend must reuse the same game code and authored scene. No direct `SDL_CreateRenderer`, `SDL_RenderTexture`, or parallel SDL 2D draw-loop fallback; using SDL for platform input/windowing under Tourmaline remains valid.
2. **Exercise the built-in GameUiRenderer.** Overlay two or three independently positioned, clickable square action icons and a numeric resource label on top of the same rendered frame. Use existing `UiLayout2D` anchors and hit-testing instead of introducing a widget framework.
3. **Have functional input/action focus.** Route mouse + two keyboard bindings to the same `GameBehaviour` command and reflect the state change in the HUD. Document the input mapping and callback/event ownership. Keyboard must work even if no mouse is present.
4. **Show how to replace the art.** A named cube mesh and a tiny source-control-safe texture/material are cooked via `.tourrecipe` into a `.pak`; game code refers to a stable asset key. Replacing the source model/material and rebuilding the assets must not require gameplay code changes.
5. **Give a deterministic, minimal iteration sequence.** Configure, generate IDs, cook content, build, run, test from a fresh clone (including `git submodule update --init --recursive`). Use the existing `tourmaline_add_project` and `tourmaline_attach_project` functions; do not hand-edit generated `project.h`.
6. **Stay minimal and cross-platform.** Prefer an executable reachable via `cmake --build <dir> --target NativeGame` (plus its documented cook target) on the repository's supported Windows/Linux toolchains. Graphics backend selection is owned by Tourmaline's CMake/runtime configuration, not by game CMake or gameplay code. Do not promise unimplemented backends; verify with Vulkan now while keeping the template ready for future Metal and Direct3D support. No Sponza/demo-content download, GI bake or special proprietary art to render the first cube. Explicitly list prerequisites that cannot yet be eliminated.
7. **Ship CPU-only checks** for game-state action transitions and catalog/recipe invariants, alongside a clearly documented optional GPU screenshot smoke test. Include actionable failures when cooked assets or shaders are missing.

An implementation that launches only an SDL rectangle/cube viewer fails this acceptance contract even if gameplay works.

## P1 polish, only after the runnable vertical slice

- Game-facing display text sample: render numeric counters and button labels using existing shipped font assets or an actual asset-pipeline font atlas. Don't require each game to rewrite glyph placement; leave shaping/complex text out of the first PR.
- Built-in example for pointer hover/pressed/disabled visual state, with a single input-to-action mapping used by mouse and keyboard.
- A default neutral lit material and a simple rough/metal texture swatch, with an obvious path to authored PBR.
- Better error messages for stale `project.h`, missing `.tapp`/`.pak` or material-path resolution.
- Optional code generation/bootstrap command **after** the sample can be copied and run manually.

## Hot reload: explicitly out of scope for this PR

**PR #84 already implements editor-scoped source watching and selective refresh**, including mesh-source and sibling-texture change handling, preservation of unsaved `.tour` overrides, and overflow recovery. Do **not** implement a second watcher, duplicate importer, or general runtime hot-reload infrastructure here.

Instead document two workflows for the starter:

- **Current reliable path:** edit glTF/PNG, recook/rebuild, restart. Make the CLI path short and deterministic.
- **When #84 is merged and verified:** show the *editor* preview/authoring loop using `EditorContentServer`, and state its boundaries honestly. Editor source refresh is **not** automatic hot replacement of shipped runtime pak assets; any in-running game reload should be a separate explicit feature request after that lifecycle is defined.

This RFC can merge before #84, and the template must not depend on the unmerged PR.

## Proposed acceptance test / review checklist

- [ ] Clone a new external game repo, init Tourmaline submodule and configure using Ninja.
- [ ] Build the engine-backed game executable and its minimal cooked scene by documented commands.
- [ ] Observe a **3D lit, textured cube** and scene background rendered by Tourmaline's `WorldRenderer` using its currently supported Vulkan backend, not SDL renderer calls.
- [ ] Observe **GameUiRenderer HUD** composited in the same frame with 3 non-row square controls and a resource counter.
- [ ] Click one square and press the corresponding key; both increment the same in-game resource.
- [ ] Replace the cube glTF or PNG, recook, relaunch; changed art appears with game logic unchanged.
- [ ] Verify external-repo build does not pull in editor, benchmarks, full repository samples or unnecessary demo assets.
- [ ] Run CPU state tests without requiring GPU; document one optional screenshot comparison via existing `tourcapture`.
- [ ] Confirm no Vulkan-specific types, Vulkan headers, backend selection logic or native Vulkan calls are present in the game's public or gameplay code; future Metal/Direct3D support should only require engine-side changes.
- [ ] Confirm no new file watcher or source hot-reload subsystem is introduced.

## Engine integration versus game responsibilities

**Engine/template:** compose runtime + backend-independent `WorldRenderer` + `GameUiRenderer` + input + stable assets + example CMake and concise documentation. The engine alone selects and implements the underlying graphics API.

**Game:** gameplay design, rules, ECS choice (or none), score model, save format, final 3D assets, specialized UI skins and music.

The intended outcome is practical: after copying the starter, game developers should be able to spend the next hour on their game instead of accidentally writing a second renderer.
