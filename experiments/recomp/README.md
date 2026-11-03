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

    cmake -S . -B out/build/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_RECOMP_DIAGNOSTIC=ON
    cmake --build out/build/recomp --target RecompDiagnostic
    cd out/build/recomp
    ./RecompDiagnostic --synthetic

On Windows use RecompDiagnostic.exe instead. Controls: WASD/left stick = stick, Space/South = A, Left Shift/East = B, Enter/Start = Start. The optional target defaults OFF. Hosted CI compiles it on the experiment branch, but GPU runtime smoke tests require a Vulkan-capable desktop session.

The diagnostic's UpdateTexture path recreates images every simulation tick: intentionally slow. A real RT64 integration needs explicit synchronized external Vulkan image interop or a persistent image upload API. The C frame fallback cannot preserve all RDP framebuffer effects or faithfully match the original timings.

## Verified upstream integration boundary

The candidate upstream n64hub/super-mario-64-recomp has SDL2/RT64 graphics startup in src/main/main.cpp, game-specific recomp replacement functions in src/game/recomp_api.cpp, static RecompiledFuncs/PatchesLib libraries in CMakeLists.txt, and tools/recompile.sh for generating native game code from a user's ROM. These are not portable adapter callbacks. Its N64ModernRuntime thread scheduler, RSP/RDP execution and RT64 Vulkan ownership must be integrated separately with Tourmaline. All upstream GPL-3.0 and game-asset rights remain separate obligations.
