# Tool CLI entry-point audit (November 2026)

This is a source-level audit of the `tools/` executable interfaces, launchers, and
related root-level scripts. **It is not a claim that the native tools have been built,
all possible callers have been traced, or every command has been exercised.**
Preserve compatibility for documented automation until callers are verified.

## Entry points reviewed

| Family | Entry points and ownership | Observations |
| --- | --- | --- |
| Asset discovery and inspection | `asset_reverse_search.py`, `asset_read_presence.py` (library), `tourinfo.py`, `tourdiff.py`, `pak_browser.py` | `tourinfo` intentionally exposes `--format` plus short output switches; `tourdiff` reuses `tourinfo.filters`; `asset_reverse_search --mesh` is an alias for `--geometry`. Do not delete aliases without checking consumers. |
| Capture, testing, benchmarks | `toursnapshot.py`, `snapshot_sequence.py`, `export_scenario_screenshots.py`, `tourcapture.py`, `golden_capture.py`, `perf_harness.py`, `pixel_compare.py`, `input_emulator.py`, `hzb_csv_to_images_v2.py`, `geometry_capture_atlas.py` | These are separate workflows (single capture, deterministic sequence, batch scenarios, pixel comparison, benchmarks). Retain explicit `legacy` capture behavior and existing `--exe` on tools that still use it. |
| Material and content utilities | `material_lab.py`, `material_lab_fixtures.py`, `ktx_bulk_convert.py`, `gen_terrain_tiles.py`, `stage_niagara_gltf.py`, `shader_od_retry.py`, `push_constants_idl/codegen.py` | `material_lab reference` always selects the `lit` view; its former `--view` override was silently ignored. |
| Project and asset host | `project_init.py`, `configure_asset_client.py`, `install_offline_assets.py`, `install_shared_assets.py`, `offline_asset_service.py`, `shared_asset_server.py`, `shared_asset_repository.py` (library) | The three installer/configuration scripts are launched by corresponding root-level `.sh`/`.bat` wrappers. Keep paths and arguments forwarded. |
| DCC exporter | `dcc/publish_gltf.py`, `dcc/maya_export_gltf.py`, `dcc/blender_export_gltf.py`; `dcc/dcc_plugins.py` and `dcc/dcc_runner.py` are support modules | `publish_gltf --runner` selects a plugin name, not a Python filename. The DCC-specific scripts are subprocess adapters. |
| Native import/build and graphics tools | `import/main.cpp`, `project/main.cpp`, `tourinit/main.cpp`, `cook/src/main.cpp`, `pak_cooker/main.cpp`, `gi_baker/main.cpp`, `cubemap_baker/main.cpp`, `reference_scene/main.cpp`, `shader_header/main.cpp` | C++ command parsers expose distinct workflows; `cook` and `pak` use positional artifact lists. Retired switches such as `--scratch-dir` deliberately give errors instead of silently changing output. |
| Windows/shell launchers | root `build.bat`, `build_app.bat`, `configure_asset_client.{bat,sh}`, `install_offline_assets.{bat,sh}`, `install_shared_assets.{bat,sh}`, `install_dependencies.{ps1,sh}`; `tools/fetch_demo_content.{ps1,sh}`, `compare_screenshots.ps1`, `optick_trace.ps1`, `cook/cook_sponza.bat` | Separate platform entry points are intentional, not duplicates to remove. |

## Findings and changes in PR #160

1. `export_scenario_screenshots.py` offered two spellings for the same executable
   (`--app` and `--exe`); keep `--app` and drop the alias. **Breaking CLI change**:
   external invocations using `--exe` must switch to `--app`.
2. `toursnapshot.py` guarded `--pixels` registration with a condition that listed
   *every* subcommand; remove the redundant branch and clarify what it validates.
3. `dcc/publish_gltf.py` described `--runner` as a Python plugin filename even
   though the loader takes a plugin name; fix help.
4. `material_lab.py reference` accepted `--view` but hardcoded the reference
   view to `lit`; reject the ineffective override and keep `--view` on
   `plan` and `run` (with regression coverage).
5. `shader_header/main.cpp` used a value-only parser and rejected
   `--help`/`-h`; make help succeed without requiring an argument.
6. `gi_baker/main.cpp` help incorrectly pointed users to `PakCooker --lightmap`;
   the lightmap stage belongs to `cook.exe`. Also explicitly document that
   `--emissive` is accepted but ignored, so it is not mistaken for a live switch.
7. `cook/src/main.cpp` described its positional CLI as `pak.exe` in
   comments; name the actual executable.

## Deliberately retained / follow-up verification

- **Compatibility:** `GiBaker --out` is intentionally parsed and *rejected*
  with a retirement diagnostic. `--gi-volume-capture-spacing` and
  `--emissive` are compatibility-only/inert; do not make them silently functional.
  Consider removing them in a separately announced breaking-change release,
  after repository and downstream callers are checked.
- **Aliases with possible consumers:** `tourinfo --format` alongside
  `--json`/`--jsonl`/`--paths`; `asset_reverse_search --mesh`;
  `Project --output`/`--out-pak`; per-tool `--exe` and `--app`.
  Identical-looking strings are not evidence of interchangeable semantics.
- **Overlapping implementations:** `tourinit` and `project_init`, as well as
  `snapshot_sequence` and `toursnapshot`, require workflow/caller and output
  comparison before any consolidation.
- **Validation still required:** run CPU-only Python tests in `tools/tests`
  and `tools/dcc`; compile native CLI targets and exercise `--help` and
  invalid/missing-argument exit codes on supported platforms; grep CI, CMake,
  documentation, and downstream scripts for removed spellings. No full build
  or native CLI execution was available during this source-only audit.

## Suggested smoke tests

```sh
python -m unittest discover -s tools/tests -p 'test_*.py'
python -m unittest discover -s tools/dcc -p 'test_*.py'
python tools/toursnapshot.py --help
python tools/material_lab.py reference --help
python tools/dcc/publish_gltf.py --list-runners
# After building the native target:
tourmaline_shader_header --help
```
