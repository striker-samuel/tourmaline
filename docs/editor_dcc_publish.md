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
  atomic file replacement (same filesystem). Failures keep the last
  working published file. The editor's content server then detects the
  published change; it does not watch or import `.ma` as geometry.
- **Unlink DCC source** deletes the authoring-link sidecar only.

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
`--runner maya|blender`. It starts the DCC-specific script with
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
