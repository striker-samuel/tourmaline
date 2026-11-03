# Native Tourmaline game starter

**Purpose:** a minimal game that uses **Tourmaline** to render a lit, textured
3D cube and a clickable, spatially arranged game HUD. It intentionally does
**not** use an SDL3 renderer, native graphics API commands in gameplay, a
second swapchain, or a bespoke graphics loop. Tourmaline currently implements
its graphics backend using Vulkan; this starter does not bind itself to that
choice.

The starter is meant to be copied into a **new, independent repository**,
not compiled in-place beneath the engine. Tourmaline stays a Git submodule.
Use this template after the engine changes in PR #85 are available.

## Bootstrap

From a new empty Git repo, copy the *contents* of
`tourmaline/templates/native_game/` to your new repo root, then register the
engine submodule with Git (copying `.gitmodules` alone does **not** create a
submodule entry):

```sh
git init
git submodule add https://github.com/striker-samuel/tourmaline.git external/tourmaline
git -C external/tourmaline checkout <engine-commit-with-native-template-support>
git add .
git commit -m "Start native Tourmaline game"
git submodule update --init --recursive
```

Install Tourmaline's pinned dependencies and toolchains using its README and
`install_dependencies.sh` / Windows setup. This project intentionally uses
the existing `Project` content pipeline, rather than copying raw glTF into
the runtime renderer.

## Configure, cook, build, run

```sh
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release
# Asset IDs can be generated independently; the executable depends on ids.
cmake --build out/build --target NativeGame_ids
# Source glTF/PNG -> .geometry/.material + game.pak + game.tapp beside exe.
cmake --build out/build --target NativeGame_paks
# Generated project.h is never edited manually.
cmake --build out/build --target NativeGame NativeGameTests
ctest --test-dir out/build -R native_game_state --output-on-failure
./out/build/NativeGame
```

The cook is ON DEMAND. After changing cube art, recook and relaunch; the
game logic keeps its asset identity. Shader compilation/staging and the
engine shader pak are handled by `tourmaline_host_setup`. A missing
`game.tapp` or missing cooked files is a fatal deployment error, not a
silent placeholder.

A platform-specific preset/build directory may differ from `out/build`.
To verify the render stack on a machine with a GPU, use Tourmaline's
existing screenshot capture flow after opening the app.

## Interaction

The screen displays a textured cube on a floor, with three **separately
positioned** floating square controls drawn through Tourmaline's
`GameUiRenderer` and the same scene frame:

| Control | Action |
|---|---|
| 1 or bottom-left square | Offer: +2 Favor and +10 score |
| 2 or upper-right square | Upgrade: spend Favor, increase passive production |
| 3 or bottom-right square | Defend: spend Favor, gain score |

A passive timer increases Favor even when the player does nothing. Mouse
and keyboard call the same pure `State::apply` function. The three actions
are intentionally small examples rather than an entire gameplay framework.

The sample owns its camera and frees the mouse cursor for the HUD. The
default GameUI coordinate mapping assumes standard 1:1 input/display
pixel scaling. A production game should account for HiDPI/UI scaling
explicitly if users enable an alternative scale.

## Asset replacement

The initial named cube, floor and light are inside
`project/content/models/placeholder/cube.gltf`. Replace the model or its
embedded PNG while retaining the source path, then run
`NativeGame_paks` and rebuild `NativeGame`. The authored scene
`project/content/scenes/main.tour` references the stable imported
`cube.geometry` identity.

This template uses a small original bitmap diagnostic alphabet that
draws through `GameUiRenderer`. Production text should use a proper
font atlas/text shaping solution rather than copying the tiny example.

## Editor hot reload

[PR #84](https://github.com/striker-samuel/tourmaline/pull/84)
already owns **editor-side source watching and refresh**. This sample
intentionally does not implement a watcher and does not advertise
editor hot reload as shipped-game hot reload.

## Known limits

- This is the source implementation. A complete native GPU build/cook and
  screenshot smoke test has **not been run** in the authoring environment.
- Backend support is controlled by Tourmaline. This starter runs on the
  currently available engine backend and does not claim Metal/Direct3D
  are implemented.
- This is a minimal composition template, not an editor, UI framework,
  audio system, ECS, production text renderer or asset hot-swap manager.
