# Linkable Tourmaline tooling (C++20)

The agent CLI tools and native applications should share the **existing
engine/project pipeline logic**, not reparse the engine formats in each app.
This directory documents the in-process, static-link API. Tooling is CPU-side:
calling an inspection or scaffolding API never opens a Vulkan device or launches
the editor.

## Build/link

From Tourmaline's CMake tree (including when Tourmaline is used as a git
submodule through `add_subdirectory`):

```cmake
add_executable(MyContentTool main.cpp)
target_link_libraries(MyContentTool PRIVATE tourmaline::tooling)
```

The target is an alias of `tourmaline_tooling` (**STATIC**). Its public include
directory is `tools/tooling/include`; its transitive links are
`project_pipeline` and `engine`. Use:

```cpp
#include <tourmaline/tooling.h>
```

**Linkage caveat:** this is a statically linked *tooling archive*, not a
promise of a wholly static executable. The engine/project pipeline still
depend on their normal Vulkan SDK, KTX, SDL and platform libraries and may
require their shared runtimes to be staged. Call sites need no Python runtime.

## Scaffold a project in-process

```cpp
#include <tourmaline/tooling.h>
#include <iostream>

using namespace tourmaline::tooling;

int main() {
    ProjectInitOptions options;
    options.root = "../projects/ForestGame";
    options.name = "Forest Game";
    options.recipeName = "forest";
    options.pakName = "world";
    options.entryScene = "content/scenes/main.tour";
    options.bakeGi = false;
    options.bakeCubemaps = false;

    ProjectInitResult result;
    std::string error;
    if (!InitializeProject(options, result, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "GUID: " << result.guid
              << ", recipe: " << result.recipe << '\n';
}
```

The implementation calls the canonical
`smvk::project::Project::Init` (and optionally
`ImportProjectAsset` / `SeedProjectScene`) so the persistent
`project.tourproj` GUID is never synthesized by a competing algorithm.
It creates the same `content/models`, `content/scenes`, `.tourmaline`
and `paks` layout as Project.exe. Default build settings are preserved.
Authored files are **never overwritten**; non-empty destinations require
explicit `adoptExisting = true`; `dryRun = true` does no writes.

A fresh scaffold has a **syntactically valid starter recipe** but no
fabricated scene/geometry. Before `Project build`, import a real source
and author/seed an actual entry `.tour` scene. The `readyToBuild` report
only means that the entry scene file exists; use `Project validate` to
confirm its dependencies and recipe ownership.

The corresponding native CLI (built as `tourinit`) is a thin adapter:

```bash
cmake --build out/build/linux-Release --target tourinit
out/build/linux-Release/tourinit \
  --name 'Forest Game' --path ../projects/ForestGame \
  --recipe forest --pak world \
  --entry content/scenes/main.tour --json

# optional import + scene seeding via the canonical project pipeline
out/build/linux-Release/tourinit --name 'Forest Game' \
  --path ../projects/ForestGame \
  --from-gltf /models/forest.glb --from-scene /models/forest.tour
```

`--adopt` is deliberate; it does not rewrite an existing GUID, recipe,
README or .gitignore. CLI returns 0 on success, 1 on initialization
failure, and 2 on argument errors. `--dry-run --json` is side-effect
free and machine-readable.

## Compose inspections and release comparisons

```cpp
using namespace tourmaline::tooling;
std::string error;
PackageSnapshot a, b;

if (!ReadPackageSnapshot("release-a/game.tapp", a, error)) {
    // error names the pak/manifest issue
}
if (!ReadPackageSnapshot("release-b/game.tapp", b, error)) {
    // ...
}
DiffOptions opts;
opts.assets = true;
opts.pages = true;
auto changed = DiffPackageSnapshots(a, b, opts);
for (const auto& row : changed) {
    // row.kind: Asset / Page / Entry
    // row.status: Added / Removed / Modified
    // row.assetBefore / assetAfter or pageBefore / pageAfter
}
```

`ReadPackageSnapshot` delegates to
`tour::asset::PakReader::OpenFile`, `smvk::LoadAppManifest`, and
`tour::asset::ParseAssetIdsBlob`. The file-backed SMP8 reader loads
only small metadata and the identity page, **not the full package**.
Request `deepVerify = true` to verify every stored page CRC at extra
I/O cost. Snapshots are owning, copied values (not borrowed page spans).
Duplicate asset IDs, old pak epochs, invalid manifests and malformed
identity tables return `false` and a clear error. All outputs are
published only on success.

`DiffPackageSnapshots` is a **pure function**: compare stable numeric
asset IDs, and compare TOC pages by (mount index, kind, path). A
content-addressed pak filename changing by itself is never a logical
asset change. A modified page's CRC does not prove which logical asset
changed. The result is a `std::vector<Difference>`; callers can compose
with `std::ranges`, predicates, JSON writers, or their own UI.

## Capture plans and decoded-pixel kernels

```cpp
CaptureOptions options;
options.executable = "out/build/App";
options.source = "content/paks/game.tapp";
options.captureFrame = 48;
options.frames = 64;
options.cameraPosition = "0,2,-4";
options.cameraForward = "0,0,1";
CapturePlan plan;
std::string error;
if (BuildCapturePlan(options, plan, error)) {
    // Spawn plan.argv using YOUR process-launch interface in
    // plan.workingDirectory with plan.environment overrides.
    // Never concatenate argv into a shell command.
}

// Pass decoded RGB byte spans from your image loader:
PixelDifference stats;
CompareRgb8(referenceRgb, capturedRgb, width, height, 6, 1,
            stats, error);
```

These are **linkable building blocks**, not an in-process GPU runner.
`BuildCapturePlan` validates and pins the execution contract; the
host owns process lifetimes and fresh-image checks. `CompareRgb8`
operates on decoded RGB8 buffers supplied by the caller; it does not
need or bundle an image decoder. For complete out-of-process screenshot
capture, fresh-frame verification and baseline manifest I/O, continue
using `tools/tourcapture.py` (Pillow-backed). It is **not yet** a thin
binding to this C++ library. Similarly, the existing `tourinfo.py` and
`tourdiff.py` remain compatibility Python frontends; the C++ API is a
second, native consumption route over the canonical engine
`PakReader`, not a claim that Python can be statically linked.

The native API is intentionally modular: scaffolding, package snapshot,
pure diff, capture-plan and pixel-compare functions can be called in
any order, independent of a CLI. The next consolidation step can replace
Python's metadata parsing with a binding to this core while preserving
the Python CLI's JSONL and filtering contract.

## Tests

```bash
cmake --build out/build/linux-Release --target ToolingApiTests
ctest --test-dir out/build/linux-Release -R '^tooling_api_tests$' --output-on-failure
```

The native test makes a real SMP8 pak and a v2 `.tapp`, exercises the
engine-backed reader and a GUID-owning project scaffold, tests filename
churn versus stable-ID diffs, and validates capture/pixel contracts.
No Vulkan *device* is needed, but normal native dependencies must be
installed for compilation/linking.
