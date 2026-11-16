# Knight demo — Animated Knight (Intel)

The **Animated Knight** demo (CC BY 4.0, see
[`../../KNIGHT-CREDITS.md`](../../KNIGHT-CREDITS.md)) — a fully rigged,
skinned character with a walk-clip animation, rendered through the GPU
scene's task/mesh skinned path. Unlike the static Bistro/Sponza demos, this
one exercises the **skinned mesh** lane (1 skin, 85 joints) plus the
**animation** clip (258 tracks, ~12.5 s).

## What is here

| path | role |
|---|---|
| `knight.tourrecipe` | the recipe build definition (`Project.exe build --recipe …`) |
| `content/scenes/knight.tour` | the scene tour (the knight reference) |
| `content/models/knight/knight.gltf` | the glTF (skin, animation, material + buffer references) |
| `content/models/knight/knight.gltf.meta` | the identity ledger (stable GUIDs across re-imports) |
| `content/models/knight/knight.mat0_Default_Material.material` | the body material |
| `content/models/knight/knight.mat1_mat1.material` | the sword/shield material |

## What is NOT here (the geometry buffer)

- `knight.bin` (5.9 MB) — the glTF's external buffer: geometry, the skin's
  `inverseBindMatrices` + packed influences, and the animation keyframes.
- `ground.bin` (120 B) — the ground-plane buffer.

Both come from the original **Animated Knight** source in Intel's GPU
Research Samples Library (see `KNIGHT-CREDITS.md`). Place them beside
`knight.gltf` under `content/models/knight/` to build.

> The buffer is deliberately not committed: the faithful Maya → glTF
> conversion is non-trivial (a naive `select -all` FBX → Blender → glTF
> round-trip splits the single skin into 36 and the 4 meshes into 7), so the
> converted `.bin` is the authoritative artifact and ships with the release
> asset set rather than on `master`.

## Build / run

The demo is a skinned, animated single-scene pak; build it directly (no GI,
no cubemaps, no LOD chain):

```powershell
# from a configured build dir (Import/Cook/Pak next to App.exe)
.\Import.exe F:/content/demo/knight_only/knight.gltf --meshlets on --meshlet-split-factor 2.0
.\Cook.exe   F:/content demo/knight_only/knight.geometry --meshlets on --meshlet-split-factor 2.0
.\Pak.exe    F:/content demo/knight_only/knight.geometry --out-pak F:/content/paks/knight.pak

# run it
.\App.exe --pak F:/content/paks/knight.pak
```

The recipe (`knight.tourrecipe`) is the `Project.exe build --recipe` form —
`import lods off` keeps the single-level chain the GPU raster path needs,
and meshlets stay on (the import default) so the scene is task/mesh eligible.

## Repro

The committed `knight.gltf.meta` records `guid`, `sourcedigest` and
`fingerprint` for the exact source set (glTF + `.bin` + ground buffer) the
release pak was cooked from, so a re-import of an unchanged source reproduces
the derivation byte-identically.
