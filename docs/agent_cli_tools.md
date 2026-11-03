# Agent CLI manual: inspection, pixels and comparisons

This is the discoverable CLI entry point for coding agents. Read it before
inspecting cooked content or interpreting rendering regression results.
Commands are run from the **repository root**. Paths below are examples.

## Principles

- Prefer headless queries. Do not launch the editor, GUI pak browser, or
  Vulkan renderer just to enumerate packaged content.
- stdout carries results; stderr carries diagnostics. Use `--json` for one
  structured document and `--jsonl` for a stream of independent records.
- Quote patterns (especially `*`, `**`, `?`, `|`) so the shell does not
  expand them before the tool receives them.
- A zero exit code means success; inspect nonzero exits rather than ignoring
  them after pipelines. Bash: `set -o pipefail`. PowerShell: check
  `$LASTEXITCODE` for native processes. Empty matches are not corruption.
- `.tapp` is an appmanifest v2 **release pointer**; `#ids` asset identities
  live in each SMP8 `.pak`. Distinguish logical assets (`assets`) from
  physical TOC pages (`entries`); one logical identity need not map to
  a single TOC page.
- Listing metadata is cheap relative to decoding payloads; `validate
  --deep` explicitly walks every stored page CRC.
- Tools do not modify source assets or cooked packages. The project
  pipeline's `Project ids` and `Project explain` may reconcile asset
  registry state: they are **not** equivalent to these read-only queries.

## tourinfo: package explorer

`tools/tourinfo.py` reuses the SMP8 and IDTR readers/decoders in
`tools/pak_browser.py`. Python standard library is sufficient for package
metadata, identities, and CRC validation; optional `pip install lz4`
is needed to inspect a compressed payload. No Qt or Vulkan needed.

```bash
python tools/tourinfo.py build/content/paks/game.tapp
python tools/tourinfo.py summary build/content/paks/game.tapp --section entry,paks,kinds --json
python tools/tourinfo.py paks build/content/paks/game.tapp --json
python tools/tourinfo.py assets build/content/paks/game.tapp --kind texture -g 'textures/**' --jsonl
python tools/tourinfo.py entries build/content/paks/game.tapp --regex 'terrain|foliage' -i --paths
python tools/tourinfo.py entries build/content/paks/game.tapp --kind mesh --sort size_stored --desc --limit 15
python tools/tourinfo.py inspect build/content/paks/game.tapp --path '#ids' --jsonl
python tools/tourinfo.py validate build/content/paks/game.tapp --deep --json
```

`-g/--glob` matches the **entire logical path**: `*` stops at `/`,
`**` crosses directories, and `**/` also matches zero directories.
`--regex` searches the logical path (Python regular expressions);
`-i` affects both. Repeated filters within a family are OR; glob,
regex and kind families combine with AND. `--kind` accepts repeated
or comma-separated names; `--fields` selects record columns.
`--sort`, `--desc`, and `--limit` constrain output.

For Unix pipelines:

```bash
python tools/tourinfo.py entries game.tapp --jsonl |
  jq -r 'select(.kind == "texture") | .path'
python tools/tourinfo.py assets game.tapp --paths |
  rg -i 'metal|roughness|normal'
python tools/tourinfo.py entries game.tapp -g '**/normal*' --jsonl |
  python tools/tourinfo.py inspect --stdin --jsonl |
  jq -c 'select(.decoded.ktxMagicOk == false)'
```

`inspect --stdin` accepts a JSONL record with `pak` and one of
`slot`, `path`, or `id`; the `entries --jsonl` output contains
both `pak` and `slot` for unambiguous inspection. Other records
may not resolve one-to-one; prefer slots when inspecting pages.

Data shape: `assets` carry catalog `id`, `kind`, `path`, `scene`,
pak name and mount index; `entries` carry page `slot`, `offset`,
`size_stored`, `codec`, `content_crc`, and VFS visibility. JSON
record commands emit `{"schema_version":1,"command":"...","items":[...]}`;
JSONL emits one record per line. A single summary/validation emits a
JSON object. The schema is v1; integrations should use field names, not
text-column positions. CLI errors are emitted to stderr, exit 2; deep
validation failure exits 1. `--jsonl` is line-delimited output, not a
promise of O(1) archive memory use: current parser reads each pak image.

The old `python tools/pak_browser.py --cli-dump|--cli-validate <path>`
remains available and is useful as a compatibility reference. The
optional GUI needs PyQt6. See `docs/pak_vnext.md`.

## Project and provenance tooling

```bash
Project validate --project ./mygame --recipe game.tourrecipe
Project status --project ./mygame
Project explain --project ./mygame --recipe game.tourrecipe --id 0x1234
```

These commands are part of the C++ project pipeline (see
`tools/project/main.cpp`, `docs/content_as_code.md`).
`Project validate` resolves ownership and mount order without cooking;
`Project status` reports imported-source freshness. `explain` resolves
the asset's origin and stale causes but **may reconcile/tombstone registry
entries**, so do not run it as a supposedly pure read-only operation.

## Pixel inspection / screenshot comparisons

Use `tools/pixel_compare.py` when comparing images cross-platform.
It uses Pillow to decode RGB pixels. PNG byte hashes are not a reliable
pixel comparison: two PNGs may encode identical pixels differently.

```bash
python -m pip install pillow
python tools/pixel_compare.py before.png after.png --json
python tools/pixel_compare.py before.png after.png --stride 1 --threshold 12 --diff-out out/diff.png
printf 'before.png\tafter.png\n' | python tools/pixel_compare.py --stdin --jsonl |
  jq 'select(.difference_fraction > 0.01)'
```

The threshold measures the *sum* of absolute RGB channel deltas per
sampled pixel (range 0..765). Default threshold is 6, stride is 2
(sample every other row and column); stride 1 examines every pixel.
`--white-min N` also counts reference pixels with every channel >= N.
JSON output includes decoded RGB SHA-256 for each image, dimensions,
sample counts, changed fraction, average delta and an optional map path.
The map marks changed sampled pixels red. Zero = no threshold-exceeding
pixel differences, 1 = differences, 2 = invalid input/IO, 3 = no Pillow.
Image dimensions must match.

**Existing Windows tool:** `tools/compare_screenshots.ps1` is
PowerShell/System.Drawing-only and remains supported on Windows for
historical runs, `-Reference`, `-Compare`, `-Threshold`, `-Stride`,
`-WhiteCount`, and `-DiffOut`. Prefer the new Python tool for
JSONL pipelines or cross-platform automation; do not silently change
the threshold or stride while comparing baseline results.

**Golden capture:** `tools/golden_capture.py` launches App to produce
a *fresh* screenshot, hashes **decoded pixels**, and compares against
GPU/driver-keyed `tools/golden_hashes.txt`. It needs a working GPU and
Pillow. Run it for GPU regression gates, not the static two-image
comparator. Update baselines only after reviewing visual differences.

**Benchmark comparison:** `tools/perf_harness.py compare --baseline
<result.json> --candidate <result.json> --max-regression 5`
compares medians of per-run average frame times while refusing
incompatible work configurations. It is not a pixel comparison. Its
`cull` mode compares exact cull verdict JSON baselines (see
`docs/perf_harness.md`).

**HZB visualization:** `tools/hzb_csv_to_images_v2.py` converts
captured HZB values to images, reports per-mip statistics, and can
verify MIN reduction. It is a diagnostic visualizer rather than an
A/B screenshot comparator; its NumPy/Pillow dependencies are separate
from `tourinfo`.

## GUI input emulator: scriptable smoke tests

`tools/input_emulator.py` accepts **JSONL event scripts** or a pipe and
defaults to **validation only**, never desktop input. Use this to script
repeatable editor/viewer interaction, with a human-controlled active window.
Validation requires only Python; live execution currently requires Windows
and `pip install pyautogui pygetwindow`.

```bash
python tools/input_emulator.py validate tools/input_example.jsonl --json
cat tools/input_example.jsonl | python tools/input_emulator.py run --stdin --dry-run --jsonl
python tools/input_emulator.py run tools/input_example.jsonl --execute --window Tourmaline
```

Actions: `key`, `key_down`, `key_up`, `wait`, `move`,
`click`, `scroll`, `text`, and `screenshot`. Sample JSONL:

```jsonl
{"action":"key","key":"tab"}
{"action":"key_down","key":"w"}
{"action":"wait","seconds":0.25}
{"action":"key_up","key":"w"}
{"action":"screenshot","path":"out/input-smoke.png"}
```

Coordinate events use desktop screen pixels. `--execute` requires
`--window` and checks the currently foreground window before **each**
action. Input scripting has fixed action/field allowlists, 1000-event
and 60-second accumulated wait budgets, no shell execution, balanced
key-down/up validation, mouse-corner failsafe and key release in a
`finally` block. Do not use it to send input to unattended or unknown
desktop windows. If the process loses focus mid-script, it fails rather
than clicking the wrong target. Live input does not work in headless CI;
use `validate` there.

## project_init: name-and-path project scaffolding

Tourmaline already owns project creation in the **native** `Project init`
command: it mints and preserves the persistent GUID in `project.tourproj`,
creates `content/models`, `content/scenes`, `.tourmaline`, `paks`,
and writes native build defaults. **Do not implement a second GUID or
marker format.** The new `tools/project_init.py` is the agent-facing
scaffolding wrapper around that canonical executable.

```bash
# Preview a new named project at an arbitrary destination (zero writes):
python tools/project_init.py --name "My Game" --path ~/games/my-game \
  --recipe-name game --pak world --bake-gi off --bake-cubemaps off \
  --dry-run --json

# Create it (choose the native Project binary from your build):
python tools/project_init.py --name "My Game" --path ~/games/my-game \
  --project-exe out/build/linux-Release/Project \
  --recipe-name game --pak world --readme --gitignore --json

# Create and import a model plus its existing authored .tour scene:
python tools/project_init.py --name "Test World" --path ~/games/test-world \
  --project-exe out/build/linux-Release/Project \
  --from-gltf /assets/world.glb --from-scene /assets/world.tour \
  --bake-gi on --json

# Explicitly adopt an existing project (keeps identity and existing files):
python tools/project_init.py --name "Existing Project" \
  --path ~/games/existing --adopt --recipe-name game --json
```

The wrapper creates `<recipe-name>.tourrecipe` with recipe grammar v1,
`entry content/scenes/main.tour` and one `[pak world]` by default.
Flags `--name`, `--path`, `--recipe-name`, `--pak`,
`--entry-scene`, `--bake-gi on|off`, `--bake-cubemaps on|off`,
`--from-gltf`, `--from-scene`, `--readme`, `--gitignore`, and
`--no-recipe` customize the scaffold. On `--from-scene`, the entry
references the native seed destination `content/scenes/<source>.tour`.
Scene seeding requires `--from-gltf` because the native implementation
repoints the imported geometry. Without a real, valid entry scene, the
recipe is **only a scaffold**: `Project validate/build` will refuse
the unresolved artifact. The CLI reports `scene_ready=false`; it
does not claim a ready-to-cook project.

New projects may target a fresh directory or empty folder. Existing
projects require `--adopt` and a matching name/valid marker GUID;
the tool **never overwrites** existing recipes, README or .gitignore.
Use `--dry-run --json` first for unattended automation.
The native binary can be passed with `--project-exe` or found on
PATH / common repo build paths. Exit 0 = scaffold succeeded; 2 =
bad arguments, missing executable, native init error or unsafe write.
A failure during native import may leave a partly initialized project
from the canonical init; inspect it rather than blindly deleting it.


## tourdiff: compare cooked releases without filename churn

`tools/tourdiff.py` compares SMP8 `.pak` files or appmanifest v2
`.tapp` releases. Asset identities are matched by persistent `#ids`
ID (kind, path, scene, owning mount index); physical pages by mount
index, kind and TOC path (slot, CRC, codec, stored length). Changing
content-addressed pak filenames alone is ignored, but mount order
changes are meaningful. Page changes do **not** prove which logical
asset changed: query `--scope assets` and `--scope pages` separately.

```bash
python tools/tourdiff.py before.tapp after.tapp --json
python tools/tourdiff.py before.tapp after.tapp --scope assets --jsonl | jq .
python tools/tourdiff.py before.tapp after.tapp --scope pages -g 'textures/**' --jsonl |
  jq 'select(.status == "modified") | {path, before, after}'
python tools/tourdiff.py before.pak after.pak --regex 'mesh|lod' -i --kind mesh
```

Use `--scope all|assets|pages` (default `all`), `-g/--glob`,
`--regex`, `-i`, `--kind`, `--status`, `--limit`,
`--include-unchanged`, `--json` and `--jsonl`. Exit 0 = no matching
changes; 1 = changes; 2 = invalid/corrupt input. A `--limit` never hides
the exit status. **CRC metadata is compared, not independently verified**:
run `tourinfo validate <path> --deep` for full integrity checks.

## tourcapture: reproducible screenshot regression

`tools/tourcapture.py` launches the built `App` or `Benchmark`
using `--disable_control --bench N` and the pinned
`TOURMALINE_CAPTURE` frame. It runs from the executable directory,
scrubs inherited `TOURMALINE_*` variables, turns off AA and auto
exposure by default and records camera/renderer pins. It refuses
stale screenshot exports; Pillow hashes *decoded* RGB pixels.

```bash
python -m pip install Pillow
python tools/tourcapture.py --exe out/build/linux-Release/App \
  --project content/paks/game.tapp --frames 64 --capture-frame 48 \
  --camera-pos 0,2,-4 --camera-fwd 0,0,1 --gpu-tag 'GPU_driver' \
  --output out/captures/base --json

python tools/tourcapture.py --exe out/build/linux-Release/App \
  --project content/paks/game.tapp --frames 64 --capture-frame 48 \
  --camera-pos 0,2,-4 --camera-fwd 0,0,1 --gpu-tag 'GPU_driver' \
  --baseline out/captures/base/capture.json \
  --output out/captures/candidate --json

jq '.comparison.difference_fraction' out/captures/candidate/capture.json
```

Output: `capture.png`, `capture.json`, `app.log`, and optional
`diff.png`. A strict `--baseline` requires matching frame budget,
capture frame, scene selection, renderer environment pins, extra App
flags and explicit GPU+driver tag. Releasing updated content remains
valid; unpinned GPU/driver baselines are refused. Alternatively
`--reference-image some.png` performs an **unverified manual** compare.
Use `--env=TOURMALINE_NAME=value` and `--app-arg=--no-dlss` for
explicit pins. `--dry-run --json` plans without launching or writing.
Existing output captures require explicit `--overwrite`.

Exit 0 = captured or equal; 1 = pixel difference or App failure;
2 = invalid/incompatible inputs; 3 = missing Pillow; 4 = no new
screenshot; 5 = timeout. Actual capture needs a real GPU/display.
Mocked capture tests in CI require no GPU.


## Suggested next agent CLIs (not yet implemented)

Ordered by value to the project; these are proposals, **not existing commands**:

1. **`tourdeps`** — query forward/reverse asset dependencies, with
   `why` and `affected` queries before recooking.
2. **`tourdoctor`** — read-only project diagnostics with structured
   source-location errors and suggested fixes.
3. **`tourshaders`** — enumerate SPIR-V bindings, metadata and variants
   without requiring a Vulkan device.
4. **`tourtrace`** — normalize Nsight, benchmark and cull artifacts into
   queryable JSONL without inventing GPU timings.
5. **`tourasset`** — bounded, typed extraction by persistent asset ID.

Keep implementations small, bounded and directly composable. Favor one
reusable core parser/decoder over multiple inconsistent pipelines.

## Testing

```bash
python -m unittest discover -s tests -p 'test_tourinfo.py' -v
python -m unittest discover -s tests -p 'test_tourdiff.py' -v
python -m unittest discover -s tests -p 'test_tourcapture.py' -v
python -m unittest discover -s tests -p 'test_project_init.py' -v
python -m unittest discover -s tests -p 'test_pak_browser.py' -v
python -m unittest discover -s tests -p 'test_pixel_compare.py' -v
python -m unittest discover -s tests -p 'test_input_emulator.py' -v
python -m unittest discover -s tools/tests -v
```

The first three are CPU-only, with image tests requiring Pillow; the
existing perf harness unit tests do not require a GPU. A production
rendering regression gate additionally needs a valid graphical
environment, matching scene and driver, and a fresh capture.
