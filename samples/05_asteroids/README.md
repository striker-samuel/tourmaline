# 05 — Asteroids over Sponza

A playable arcade Asteroids sample for Tourmaline, using the existing **Sponza 3D scene** as a backdrop and an independent **GameUiRenderer** gameplay layer. The 3D renderer, game simulation, shader and texture atlases remain separate. No third-party game assets are downloaded at runtime.

The game uses two **original CC0 pixel-art PNG atlases** plus authored **SpriteSet metadata** for the ship, asteroid animation clips, projectiles, effects and custom 5×7 glyph font. The `.spriteset` files are parsed by the [first-class SpriteSet API in PR #55](https://github.com/striker-samuel/tourmaline/pull/55); the game demonstrates grid expansion, named animation sampling and normalized UV regions. The PNGs remain host-owned textures. Source images and the reusable atlas-cell mapping are in [assets/](assets/README.md). The shader `shaders/asteroids_gameui.frag.hlsl` is a project-local HLSL example providing **2/4/8-texel GPU sprite pixelation**, plus switchable CRT scanlines and vignette on the translucent playfield. Its optional effect payload travels through the unused word of the GameUI instance ABI. It is compiled into `content/shaders/asteroids_gameui.frag.hlsl.spv` and enabled via `SetGameUiFragmentShaderPath`. The game also opts into nearest-neighbor sampling for the pixel-art rectangles. Font glyphs are textured, one instance each — no runtime font dependency.

Sponza data is **not** included in this sample. The rolling `demo_content` release provides `sponza_10.tapp` and its content-addressed pak. See [Sponza credits and licensing](../../SPONZA-CREDITS.md) before distributing those assets.

## Build and run

Follow [the root build instructions](../../README.md#getting-started). From the repository root:

```sh
cmake --build out/build/linux-Release --target demo_content
cmake --build out/build/linux-Release --target Asteroids
cd out/build/linux-Release
./Asteroids
```

To replay a run with Tourmaline's **new engine-owned deterministic RNG**, set
`TOURMALINE_RNG_SEED` at launch (the host prints the effective seed).
The sample takes its own named `asteroids` PCG32 stream via
`app::Context::random().MakeStream("asteroids")`, independent of other
random consumers. Starting a new game restores the stream checkpoint,
so a given seed yields the same first wave; this only guarantees seeded
RNG, not full frame-by-frame replay or identical physics across platforms.

```sh
TOURMALINE_RNG_SEED=42 ./Asteroids
```

Tourmaline's new **input tape** support (merged PR #68) can replay the
same keystrokes and simulation-frame timesteps. Combine it with a fixed
RNG seed to reproduce asteroid waves. Create the output directory before
recording; recordings are never overwritten automatically:

```sh
mkdir -p captures
TOURMALINE_RNG_SEED=42 ./Asteroids --input-record captures/asteroids.tinp
TOURMALINE_RNG_SEED=42 ./Asteroids --input-replay captures/asteroids.tinp
```

Replays stop automatically when the tape finishes. Use a previously recorded
file and the **same seed/content/build** for useful comparisons.
The engine guarantees normalized input-event replay and deterministic RNG,
**not** bit-exact physics, audio or Vulkan image output. See
[docs/input_tape.md](../../docs/input_tape.md).

For Windows use `out/build/x64-Release` and `Asteroids.exe`. CMake stages `sprites.png`, `font.png` and their two `.spriteset` metadata files into `content/asteroids` beside the executable. It also generates original `audio/music.mid`, a matching loopable `audio/music.wav` and four WAV sound effects with pure Python; the custom shader is compiled to the exe-relative `content/shaders` directory. Run the executable **from its build directory**. The default manifest is `content/paks/sponza_10.tapp`; you may override with `--project path/to/other.tapp`. Do not hardcode a content-addressed pak name.

## Controls

| Key | Action |
| --- | --- |
| Enter | Start or restart after game over |
| Left / Right | Rotate ship |
| Up | Thrust |
| Space | Fire |
| Esc | Quit |
| Tab | Tourmaline debug panel |
| 5 | Cycle sprite pixelation: off / 2 / 4 / 8 texels |
| 6 | Toggle scanlines and CRT vignette |

Destroy large asteroids to split them into smaller ones, survive successive waves and earn points. You have three lives and a brief invulnerability period after respawning.

## Implementation and tests

`AsteroidsGame.h/.cpp` contains C++20 fixed-step game simulation without graphics dependencies. `PixelArt.h/.cpp` decodes the staged RGBA PNGs, uploads them as Tourmaline textures and submits instanced rectangles with atlas UV coordinates. The game parses the authored SpriteSet grids and animations, loads textures in `GameBehaviour::onStart` and releases them in `onStop` while the Vulkan resource manager is still alive. The standalone sample reads its staged loose `.spriteset` files directly; projects can instead pack them into the catalog through PR #55 and resolve them by `project::spritesets` typed keys. If sprite textures fail to load, the previous vector shape fallback remains; missing assets are logged to stderr.

`WorldRenderer::SetExtraGameUi` composites the independent game UI pass over the Sponza scene without introducing another swapchain or ImGui context. The sample's font is an image atlas rather than a TrueType/OTF font.

**Retro audio:** `generate_audio.py` authors the melody from note sequences and emits both a standard MIDI file (editable in a DAW) and 22.05 kHz mono WAV for playback via the existing Tourmaline `AudioEngine`—no runtime MIDI synthesizer or soundfont. Laser, hit, wave and game-over effects are also generated, and playback gracefully disables when audio is unavailable. The audio, melody and artwork are original and released under CC0; C++/HLSL remain under their file licenses. Music starts at gameplay start, loops, and stops on game over or shutdown.

**Dependency:** [PR #55 (SpriteSet)](https://github.com/striker-samuel/tourmaline/pull/55) must be integrated before this sample can compile. The renderer in [PR #43](https://github.com/striker-samuel/tourmaline/pull/43) has already been merged into master.

Run `cmake --build <build-dir> --target asteroids_sim_tests asteroids_spriteset_tests` and `ctest --test-dir <build-dir> -R 'asteroids_' --output-on-failure` with `BUILD_TESTING=ON` to check the pure gameplay simulation, authored SpriteSet metadata, original MIDI/WAV generation and PNG atlas contracts. Interactive rendering requires a working Vulkan 1.3-capable GPU and desktop. Build/shader compilation and GPU smoke tests still need to be run in the full Tourmaline toolchain.


## New headless diagnostics on master

The engine's agent CLI tools can inspect the Sponza manifest without a GPU:

```sh
python tools/tourinfo.py summary out/build/linux-Release/content/paks/sponza_10.tapp --section entry,paks,kinds --json
```

For automated pixel smoke tests, see `docs/agent_cli_tools.md` for
`tools/tourcapture.py`, `tools/pixel_compare.py` and the input
emulator. The game itself does not require these CLI tools at runtime.
