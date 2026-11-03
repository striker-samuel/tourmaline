# External DCC editing and basic glTF publishing

Tourmaline keeps **authoring source**, **published source representation**,
**derived editor artifacts**, and **runtime paks** separate:

```text
Maya scene .ma/.mb (or Blender .blend) [authoring source]
  -> DCC export runner (build process)
  -> staged embedded .gltf (validation + atomic replace)
  -> <project>/content/.../<asset>.gltf [published representation]
  -> EditorContentServer [settled OS change]
  -> ImportMeshArtifactFor [stale-gated .geometry/.material/.anim]
  -> ReloadReferencesViaTemp [visible refresh; unsaved .tour edits survive]
  -> optional cook/pak [separate explicit project build]
```

This **does not** confuse a Maya document with a glTF importer input.
The engine importer still supports glTF/GLB/OBJ only; a .ma/.mb cannot be
loaded directly into a Tourmaline world.

## Editor actions

In the **Assets** panel header or an asset tile's context menu:

- **Open in DCC** opens the native linked document using its OS file
  association. When no link exists, opens the published .obj/.gltf/.glb
  (or image). Clicking a manifest-derived .geometry/.material/.anim
  resolves to its owning mesh source first.
- **Link DCC source...** records a native authoring document beside the
  selected **published asset** as `<asset.gltf>.dccsource`, with an
  optionally relative path. It can point outside the project.
- **Publish glTF** runs the DCC build adapter on the editor's background
  worker pool. The chosen source is saved in its DCC before publishing.
  The output is a validated **embedded glTF 2.0** published with one
  atomic file replacement (same filesystem). The temporary glTF has
  a `.tmp` filename so the content watcher ignores it. Failures keep the last
  working published file. The editor's content server then detects the
  published change; it does not watch or import `.ma` as geometry.
- **Unlink DCC source** deletes the authoring-link sidecar only.

A native DCC tile never overwrites an existing, unrelated glTF just
because it has the same stem; an existing output must already be linked
to that native file or be explicitly linked from the glTF tile.

**First publication:** place `hero.ma` (or `hero.blend`) under the
project's content tree, select it and choose **Publish glTF**. The
runner creates `hero.gltf` beside the native file and records the
DCC link after a successful build. Subsequent edits can be triggered
from the native source, published .gltf or a derived .geometry tile.
For an existing OBJ you can also select **Open in DCC**, edit/save
the OBJ in its associated application, and let the existing watcher
refresh it. The basic automated exporter targets **.gltf**, not OBJ;
switching an existing OBJ asset's identity to glTF is not implicit.

## Basic build runners

Editor CMake stages `tools/dcc` beside the executable. The
`publish_gltf.py` CLI accepts `--source`, `--output`, and
`--runner <plugin-name>` (built-ins: `maya` and `blender`).
The plugin runner starts the DCC-specific script with
argument arrays (no shell), checks its exit status, validates glTF
version and embedded dependency URIs, and atomically replaces the
published representation. No partial export overwrites the prior
publication.

- **Maya:** set `TOURMALINE_MAYAPY` to the path to `mayapy`, and
  install/configure a compatible Python glTF exporter in Maya's
  environment. The sample `maya_export_gltf.py` calls
  `glTFExport.export(..., resource_format="embedded")` from the
  community **maya-glTF** exporter. Maya does *not* supply this as a
  built-in glTF exporter; a studio may replace this adapter with its
  supported Maya exporter or a Maya FBX -> glTF converter.
- **Blender:** set `TOURMALINE_BLENDER` to the Blender executable
  (or use `blender` on PATH). The supplied adapter exports
  `GLTF_EMBEDDED` through Blender's glTF exporter.
- **Python:** set `TOURMALINE_PYTHON` for the editor's publisher CLI
  (default `python` on Windows and `python3` on Linux).

Example without Editor:

```sh
python tools/dcc/publish_gltf.py \
  --source /project/content/models/hero.ma \
  --output /project/content/models/hero.gltf \
  --runner maya
```

## Python plugin jobs (Deadline-style local executor)

The native Editor is a *client*: it still calls `Publish glTF` through its
worker pool. The Python publisher now discovers trusted DCC export plugins
by filename and builds an explicit **process job** rather than hard-coding
Maya/Blender in the publish transaction.

```text
tools/dcc/
  publish_gltf.py         # publish transaction: stage -> validate -> replace
  dcc_plugins.py          # find/load trusted Python plugin files
  dcc_runner.py           # ProcessJob + run_local (no shell)
  plugins/
    maya.py               # mayapy + Maya environment + script/args
    blender.py            # blender --background + --python + script/args
  maya_export_gltf.py     # the script that runs INSIDE mayapy
  blender_export_gltf.py  # the script that runs INSIDE Blender
```

A plugin describes a `ProcessJob`: job name/software, executable,
executable arguments, optional script switch, Python script, script
arguments, environment overrides, working directory, timeout, priority,
and tags. `run_local` starts the executable with the merged job
environment and argument **array**, waits for completion, and propagates
the exit code and a bounded output diagnostic. No shell string is built
from paths or user input. The same job description can be consumed by a
future Deadline/farm executor; **this PR does not submit farm jobs**.

To add a DCC, place a studio-managed Python module
`tools/dcc/plugins/<name>.py` exporting `Plugin(ExportPlugin)`. Set
`name`, `software`, `extensions`, optional `priority`/`tags`,
and implement `build_job(source, staged, tools_dir, *, executable,
environment)`. No switch/case needs to be added to
`publish_gltf.py`. Plugin files are **trusted pipeline code shipped
with the editor**, not untrusted scripts loaded from asset folders.

```sh
# List installed Python export plugins (no DCC required)
python tools/dcc/publish_gltf.py --list-runners

# Override the DCC executable and the per-job environment:
python tools/dcc/publish_gltf.py \
  --runner maya \
  --source "/project/content/models/hero.ma" \
  --output "/project/content/models/hero.gltf" \
  --dcc-exe "/opt/autodesk/maya/bin/mayapy" \
  --env "MAYA_PLUG_IN_PATH=/studio/maya/plugins" \
  --env "PYTHONPATH=/studio/pipeline/python" \
  --timeout 1800
```

`--env NAME=VALUE` is repeatable and only modifies the **child**
environment. The Maya plugin also disables Maya's customer feedback /
error-reporting startup popups by default. Blender uses the same
executor with `--background <scene> --python <script> -- <output>`.
The publish stage remains atomic and is the only writer to the
published `.gltf`: the content server sees a complete replacement and
reimports as usual.

**Regression tests:** `python -m unittest discover -s tools/dcc
-p 'test_*.py' -v` covers plugin discovery, environment/argument passing,
process failure/timeout, bad exports preserving the previous publication,
and a real fake-Python-executable child, without Maya or Blender installed.
The Tests GitHub workflow runs them on Linux and Windows.

## Shared registry boundary

The existing `tools/shared_asset_repository.py` stores **immutable source
revisions** and **published product representations** independently of
the editor's derived/packed content IDs. This first integration
publishes the glTF **locally into the project**, where the content
server can observe it. It does **not** upload the `.ma` source or
the glTF product to the shared NAS automatically. Linking it to the
shared product-publish API requires a known source asset ID and source
revision (see `AssetClient.publish_product` in
`tools/maya/tourmaline_asset_cache.py`), plus policy for when an
authoring source becomes a new revision.

## Verification

```sh
python -m unittest discover -s tools/dcc -p 'test_*.py' -v
cmake --build <build-dir> --target AssetOpsTests EditorContentServerTests Editor
ctest --test-dir <build-dir> -R 'asset_ops_tests|editor_content_server_tests' --output-on-failure
```

Test manually: select `hero.ma`, run Publish glTF, open the published
scene/derived geometry, edit+save the Maya scene and republish. Confirm
the viewport updates after the content server settles, the derived
assets retain GUIDs, and the active scene keeps unsaved `.tour`
edits. Also verify a failed export does not alter the last known good
publication.
