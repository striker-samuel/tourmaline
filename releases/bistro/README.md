# Bistro demo — recreate-from-source package

The **Amazon Lumberyard Bistro** exterior demo (CC BY 4.0, see
[`../../BISTRO-CREDITS.md`](../../BISTRO-CREDITS.md)). The cooked pak is
~1.4 GB and the source textures are ~700 MB, so **neither is committed
here**. What is committed is the small *definition* — the recipe, the
scene tour, the glTF, the identity ledger and the derived material
assets (~2 MB) — enough to define the demo and, given the ORCA source,
recreate it.

## What is here

| path | role |
|---|---|
| `bistro_static.tourrecipe` | the recipe build definition (`Project.exe build --recipe …`) |
| `content/scenes/bistro_static.tour` | the scene tour (static-root reference + `setStatic`) |
| `content/models/bistro_static/bistro_static.gltf` | the glTF (materials, lights, texture URIs, geometry buffer references) |
| `content/models/bistro_static/bistro_static.gltf.meta` | the identity ledger (stable GUIDs across re-imports) |
| `content/models/bistro_static/*.material` | the 254 derived material assets |

## What is NOT here (obtained from ORCA)

- `bistro.bin` (96 MB) — the glTF's external geometry buffer.
- The 343 `.ktx2` source textures under `objects/` and `textures/`
  (~700 MB) — the maintainer's KTX2 conversion of the ORCA textures.

Both come from the original
[Amazon Lumberyard Bistro on NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro).

## Recreate

1. Download the ORCA Amazon Lumberyard Bistro (FBX + textures).
2. Convert the FBX to glTF (any DCC — Blender works) and the textures to
   KTX2, matching the `bistro_static.gltf` URI layout committed here.
3. Place `bistro.bin` and the `.ktx2` textures under
   `content/models/bistro_static/` beside the committed glTF.
4. Cook it (from a configured build):
   ```powershell
   # Windows
   .\out\build\x64-Release\Project.exe build `
     --project releases/bistro --recipe releases/bistro/bistro_static.tourrecipe `
     --publish releases/bistro/build --manifest-name bistro_static
   ```
   or use `tools/recreate_bistro.ps1` / `tools/recreate_bistro.sh`, which
   verify the source and drive the same cook.

The recipe has `bake gi off` / `bake cubemaps off` — the exterior authors
no GI volumes or cubemap captures, so those stages exit clean.
