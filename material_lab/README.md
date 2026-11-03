# Material Evaluation Lab

A CLI + source-fixture lab for evaluating material appearance across several
lighting environments through Tourmaline's **production Vulkan renderer**.
It reuses `tools/tourcapture.py` for fresh-frame capture and strict GPU/driver
baseline comparisons, plus the existing `TOURMALINE_DEBUGVIEW` PBR diagnostics.
It does not substitute the editor's approximate matball shader for real shading.

This initial PR is a headless/CLI harness and generated content fixtures,
**not** a new in-editor material inspector.

## Environments

| ID | Included fixture generator? | Purpose |
| --- | --- | --- |
| `studio` | Yes | Neutral analytic sky and directional light |
| `cornell` | Yes | Cornell-style red/green-walled room with point light |
| `outdoor` | Yes | Outdoor analytic sky and sun |
| `ibl_softbox` | External only | Real, baked softbox probe IBL |
| `ibl_overcast` | External only | Real, baked overcast probe IBL |
| `ibl_warm_indoor` | External only | Real, baked warm-interior probe IBL |

The bundled sky environments use **analytic sky IBL**, not photographic HDR
maps. Cornell is a diagnostic enclosure, **not** physically calibrated
path-traced ground truth. The three `ibl_*` slots are excluded from default
runs until an artist/developer supplies distinct, cooked cubemap/IBL scenes
with those names. Setting `TOURMALINE_CUBEMAP=1` does not create a real HDRI.
The existing renderer's procedural fallback is not an HDRI reference;
inspect your baked probe installation before recording baselines. Respect
third-party HDRI licensing and attribution.

## Generate and cook the fixtures

Build `Project` and `App` normally for your platform (see root README).
From the repository root, generate source assets:

```sh
python tools/material_lab_fixtures.py --project out/material-lab/source
```

This makes a project marker, three .gltf/.bin source pairs under
`content/models/<environment>/`, and three .tour scenes under
`content/scenes/`. Each scene shares the same 3x3 sphere layout.
The materials include dielectric and metallic roughness extremes,
copper, an intermediate metallic case, and deliberate roughness/albedo
warnings. The case camera positions live in `material_lab/suite.json`.
The generator also writes `.tourmaline/build-settings.txt` with
`cubemap=off`: these are analytic-sky environments, so Cook's baked
capture stage must not run (the auto probe lands inside the centre
sphere and the pak gate refuses the resulting empty capture manifest).
Keep that setting off unless you deliberately switch a suite to baked
probe references.

Import the source glTFs into the native geometry family, then cook each
scene into its own pak. Linux example:

```sh
BUILD=out/build/linux-Release
PROJ=out/material-lab/source
cmake --build "$BUILD" --target Project App
for env in studio cornell outdoor; do
    "$BUILD/Project" import --project "$PROJ" \
        --source "$PROJ/content/models/$env/$env.gltf"
    "$BUILD/Project" build --project "$PROJ" \
        --scene "content/scenes/$env.tour" \
        --output "$BUILD/content/paks/material_lab_$env.pak"
done
```

On Windows run equivalent commands in Developer PowerShell using
`out/build/x64-Release/Project.exe`. The paks are consumed relative to
the `App` executable working directory, as in `tourcapture`.
The bundled suites address their scene as `"scene"` — the name the
single-scene legacy cook stamps into the pak (scenes load by cooked
identity, and a miss fails loudly). If your published scene names
differ, update the suite's source `scene` field. Generated assets and
captures should remain under `out/` rather than being committed.

**GPU smoke steps still need executing on a Vulkan-capable development
machine.** The CPU unit tests do not prove the glTF cook, lighting, camera
or rendered pixels look correct. In particular, inspect the Cornell light
and probe behavior visually before considering any image a golden reference.

## Capture and compare

Inspect the suite, then plan a small subset without a GPU:

```sh
python tools/material_lab.py list
python tools/material_lab.py plan --environment studio,cornell \
    --material metal_smooth,copper_brushed --view lit,health
```

First capture:

```sh
python tools/material_lab.py run \
    --exe out/build/linux-Release/App \
    --environment studio,cornell,outdoor \
    --material metal_smooth,copper_brushed --view lit,health,roughness \
    --gpu-tag 'GPU-and-driver-id' --output out/material-lab/baseline
```

Compare a later capture on the *same GPU and driver*:

```sh
python tools/material_lab.py run \
    --exe out/build/linux-Release/App \
    --environment studio,cornell,outdoor \
    --material metal_smooth,copper_brushed --view lit,health,roughness \
    --gpu-tag 'GPU-and-driver-id' \
    --baseline-dir out/material-lab/baseline \
    --output out/material-lab/candidate
```

Use `--suite path/to/suite.json` **before** the subcommand to select a
different suite. Select an external IBL explicitly, e.g.
`--environment ibl_softbox`, only after the actual cooked asset exists.

Each case writes `capture.png`, `capture.json`, `app.log`, and
`case.json` in its `environment/material/view` directory.
Comparison cases also get `diff.png`. A suite produces `report.json`
and, with Pillow, `contact-sheet.png`.

`tourcapture` pins the frame number, camera, disabled AA/auto exposure
defaults, and GPU/driver identity, and checks decoded pixels rather than
PNG byte encoding. The suite adds a case signature over scene path,
camera, view, renderer pins, app arguments and tolerances. Missing or
incompatible case metadata is rejected **before launching** the renderer.

By default, `threshold=6` is an RGB L1 per-pixel tolerance, and
`max_difference_fraction=0` means no threshold-exceeding pixels allowed.
A manually increased difference fraction only accepts a real compared
capture, never a missing or failed renderer run. The initial run is
reported as **captured**, not as a passed regression. PBR Health green
means configured heuristics pass, not proof of physically accurate
material properties.

## Tests

```sh
python -m unittest discover -s tools/tests -p 'test_material_lab.py' -v
python -m compileall -q tools/material_lab.py tools/material_lab_fixtures.py
```

The CPU suite checks matrix selection, metadata/pin validation, baseline
rejection, no-launch planning and generated glTF geometry consistency.
Existing `.github/workflows/perf_scenarios.yml` runs `tools/tests` in
GPU-less CI. To validate GPU behavior locally, cook each bundled scene,
capture `lit` and `health` for a known sphere, repeat with a strict
baseline, and visually inspect resulting contact sheets.
