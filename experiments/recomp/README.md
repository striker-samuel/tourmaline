# Tourmaline x Super Mario 64 recomp — exploratory port-kit

STATUS: Not for merge. NOT a playable game port. This is a tested host
ABI proposal that intentionally contains no Nintendo game content and
no GPL recomp runtime.

Reference: https://github.com/n64hub/super-mario-64-recomp
The existing SM64 recomp runs with N64Recomp, N64ModernRuntime and RT64.
Its GPL-3.0 source/runtime is intentionally NOT included in Tourmaline's
MIT tree; a separate upstream checkout and appropriate license review
are prerequisites for a combined distribution.

Implemented: N64 ROM byte-order normalization (z64/v64/n64), portable
SHA-1, strict SM64-USA ROM identity check, 8 MiB big-endian guest memory
and cached/uncached aliases, controller bitmask and analog conversion,
30-Hz fixed simulation tick with bounded catch-up, RGBA8 frame copy
with row-pitch checks, persistent user-selected EEPROM storage,
and a versioned host-to-external-recomp callback table (boot/step/
capture/shutdown). The session never boots an invalid ROM and never
ships compiled copyrighted game output.

Run CPU tests without any game content or Vulkan dependencies:

    cmake -S experiments/recomp -B out/portkit -G Ninja
    cmake --build out/portkit
    ctest --test-dir out/portkit --output-on-failure

For a future real port, implement tour::recomp::Bridge using the
externally built recomp project. Supply your own legitimate SM64 USA
8 MiB ROM (normalized SHA-1:
9bef1128717f958171a4afac3ed78ee2bb4e86ce) at a path outside
source control. Save files also live in user-owned storage.

Remaining work BEFORE a playable game: wire N64ModernRuntime entry
points and guest overlays; use RT64 for real RSP/RDP display lists,
shader semantics and framebuffer effects; provide synchronized GPU
image/command interop with Tourmaline's render graph (the current
CPU RGBA fallback is only diagnostic); SDL3 controller mapping and
rumble; realtime audio; save callbacks at game-expected boundaries;
interpolation and widescreen tuning; native GPU tests and privately
run real-ROM smoke tests. None of those are claimed complete.

Design lesson: expose native host lifecycle, guest memory, input/audio
and storage, GPU external image import and pre/post-composition hooks,
not Tourmaline WorldRenderer's internal push constants or .tour scenes.


## C ABI and native diagnostic host

- recomp_abi.h is a C17-compatible versioned ABI with separate boot, step, RGBA frame capture, shutdown and optional audio/rumble/log callbacks.
- BridgeRunner tests the adapter with synthetic bytes. The SM64 Session wrapper additionally validates the 8 MiB USA ROM and SHA-1 before starting it.
- host/diagnostic_host.cpp optionally runs a synthetic frame producer through Tourmaline's own SDL3 App, Vulkan ResourceManager, RenderGraph and GameUiRenderer. **This shows only synthetic color patterns, never SM64 gameplay.**

Configure the full Tourmaline source using the usual KTX, Embree and Vulkan dependencies, with:

    # BUILD is an external directory, e.g. "$HOME/tourmaline-recomp-build"
    cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_RECOMP_DIAGNOSTIC=ON
    cmake --build "$BUILD" --target RecompDiagnostic
    cd "$BUILD"
    ./RecompDiagnostic --synthetic

On Windows use RecompDiagnostic.exe instead. Controls: WASD/left stick = stick, Space/South = A, Left Shift/East = B, Enter/Start = Start. The optional target defaults OFF. Hosted CI compiles it on the experiment branch, but GPU runtime smoke tests require a Vulkan-capable desktop session.

The diagnostic's UpdateTexture path recreates images every simulation tick: intentionally slow. A real RT64 integration needs explicit synchronized external Vulkan image interop or a persistent image upload API. The C frame fallback cannot preserve all RDP framebuffer effects or faithfully match the original timings.

## Verified upstream integration boundary

The candidate upstream n64hub/super-mario-64-recomp has SDL2/RT64 graphics startup in src/main/main.cpp, game-specific recomp replacement functions in src/game/recomp_api.cpp, static RecompiledFuncs/PatchesLib libraries in CMakeLists.txt, and tools/recompile.sh for generating native game code from a user's ROM. These are not portable adapter callbacks. Its N64ModernRuntime thread scheduler, RSP/RDP execution and RT64 Vulkan ownership must be integrated separately with Tourmaline. All upstream GPL-3.0 and game-asset rights remain separate obligations.


## Parity tracing

trace.h provides a bounded FrameTrace of 30-Hz tick index, N64 pad state, RGBA FNV-1a64 hash and frame dimensions. Traces can be exported to CSV or compared to locate the first differing retained frame. It deliberately stores neither framebuffer pixels nor any ROM data. This is a diagnostic building block; matching image hashes does not by itself prove accurate audio, physics or RDP behavior.


## Tourmaline-managed assets (external user project)

The user-controlled ROM is **never imported into Tourmaline**. It remains
an absolute external file path, validated by `Session::Boot` before an
independent static recomp adapter can execute it. No ROM or Nintendo
extracted assets are checked in. **There is no SM64 asset extractor in
this experiment**; conversion from any original game data into
Tourmaline-supported formats is a separate, user-local process.

The supported content path is now the *existing* Tourmaline pipeline:

1. `Project.exe init/import` creates an external project and imports a
   nonproprietary one-triangle bootstrap scene, because recipes currently
   require a scene entry (there is no asset-only appmanifest entry).
2. The user places already-converted assets under the **external**
   `content/extracted/` directory. Supported extension classes include
   images (.png/.jpg/.jpeg/.tga/.bmp/.ktx2), .wav, compiled .spv,
   .audiograph, .spriteset, and .givolumemanifest. Unknown types fail
   loudly. This does not magically transform N64 models, textures or
   audio into Tourmaline-ready files.
3. `Project.exe validate/ids/build` owns recipe resolution, stable ID
   allocation in assets.db, the generated catalog header, cooking,
   CRC-verifiable SMP8 paks and the .tapp release manifest.
4. The host uses **ProjectRuntime::Load** and **ReadAsset(id)**, hence
   the shared ContentRegistry provider selection and pager. It does not
   read loose assets directly or reimplement a packer. The C ABI's
   `asset_read` callback exposes a two-pass, caller-owned byte copy by
   numeric catalog ID to a separately built adapter.

Example (all `$BUILD`, `$CONTENT`, `$PUBLISH`, `$ROM`, and `$RECOMP_OUT`
paths **outside** the Tourmaline checkout):

```sh
# From the Tourmaline source checkout. BUILD, CONTENT, PUBLISH,
# RECOMP_OUT and ROM must all resolve OUTSIDE this checkout.
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTOURMALINE_RECOMP_DIAGNOSTIC=ON
cmake --build "$BUILD" --target Project RecompDiagnostic
python3 experiments/recomp/scaffold_external.py bootstrap \
  --root "$CONTENT" --project-tool "$BUILD/Project" \
  --rom "$ROM" --artifacts "$RECOMP_OUT"

# User independently converts their owned game assets into one of
# Tourmaline's supported source formats, and places them in:
# "$CONTENT/content/extracted/" (possibly with subdirectories).

python3 experiments/recomp/scaffold_external.py build \
  --root "$CONTENT" --project-tool "$BUILD/Project" \
  --publish "$PUBLISH" --rom "$ROM" --artifacts "$RECOMP_OUT"

# The ID comes from $CONTENT/generated/project.h. This is a SYNTHETIC
# diagnostic display, not SM64 gameplay: loaded asset bytes change
# the generated test pattern (via a hash).
cd "$BUILD"
./RecompDiagnostic --synthetic \
  --assets "$PUBLISH/recomp_assets.tapp" --asset-id 0x2000
```

On Windows use `python`, `Project.exe` and
`RecompDiagnostic.exe`; the commands and paths are illustrative.
**Do not assume `0x2000` is the generated ID** — inspect the
generated `project.h` and pass the value actually allocated for an
extracted asset. An optional `--assets ...` without
`--asset-id` verifies opening the project session alone.

External path policy is enforced by `external_assets.h` in C++ for the
diagnostic host and by `scaffold_external.py` in Python. They reject
paths inside the checkout, including symlink-resolved aliases and
prefix-lookalike mistakes. The pipeline itself can still ingest
improperly placed data if invoked **directly without this wrapper**;
the guard applies to the experimental commands, not arbitrary
Tourmaline tools. Saves and native recomp output remain user-owned.

**Unsolved for a real game:** mapping game-specific resource requests
to generated catalog IDs, cooking unsupported N64-specific material and
geometry formats, validating extracted content, and integrating the
external N64ModernRuntime/RT64 rendering/scheduling stack. The
diagnostic host intentionally proves content access and presentation
only, not a playable port.
