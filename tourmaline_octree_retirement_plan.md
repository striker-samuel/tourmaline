# Tourmaline: retire the legacy SG-GI octree

Target: world tiles + authored convex `.givolume` GI, with regular GI volumes retained as their own fallback. Legacy `.sggi` is removed in stages to avoid deleting an asset that build/cook still requires.

## Stage 1 — stop using the octree (accompanying patch)

Files: `engine/include/Engine/Renderer/WorldRenderer.h`, `engine/src/Renderer/WorldRenderer.cpp`.

- Do **not** invoke `LoadGiOctree()` at scene load. Explicitly reset octree CPU/GPU state on scene rebind so `frame.giEnabled` stays off.
- Change `GetActiveGIVariant()` so convex GI volumes select `GIVariant::Full` **without** `m_giLoaded`. The existing `GI_FULL` shader handles the no-octree case via sky fallback when `frame.giEnabled == 0`.
- Register only WorldFogDensity (field 0); do not register world-field mean/coverage providers that generate bricks by sampling the removed CPU octree. The volume mean textures are independent; `SampleFogGi` handles the missing world-field fallback.
- Make World Map (C) draw the loaded convex GI volume XZ lattices (one rendered dot per 6-pixel screen cell), and report total 3D lattice sites and drawn XZ dots. Do not use `m_giOctree.probes`.
- Replace the legacy octree statistics with the actual convex and regular GI-volume counts.

**Preserved:** `.givolume` load path, regular volume path, environment world tiles, reflection cubemaps, fog density world field.

**Intentionally not removed yet:** legacy loader implementation; `.sggi` file parsing and baking; pak binding enums; GI_OCTREE shader permutation; CPU fog-GI source classes; old octree UI/debug diagnostics elsewhere. These still compile but are not active during scene load.

## Stage 2 — stop generating and requiring `.sggi`

1. Search the build pipeline and project templates for `.sggi`, `--out`, `giAsset`, `GiBinding::Kind::Probe`, `m_giProbeName`, `LoadGiOctree`, `SgGiVolume`, and `gi_baker`. Identify **which producers** create `.sggi` and **which consumers** insist on it, especially `Project.exe build`, `GiBaker`, `PakCooker`, `ResolveGiBakes`, project manifest validation, and stale-bake/freshness checks.
2. Make an authored `GiVolume` binding sufficient for a freshly cooked pak. Do not demand a `Probe` binding or a `.sggi` artifact. For worlds without authored GI volumes, retain normal sky and/or regular-volume fallback; do not silently require the old octree.
3. Stop scheduling octree bakes. Remove octree-specific generator outputs from bake manifests, freshness metadata and the VFS asset registry. Keep *per-volume* `.givolume` bakes and cubemap/reflection outputs.
4. Bake a clean scene to a **new** output folder. Confirm that build/cook succeeds with no `.sggi` present. This is the gate before code deletion.

## Stage 3 — delete dead octree implementation

- Remove the now-unused `WorldRenderer::LoadGiOctree()` definition/declaration, `m_giOctree`, `m_giLoaded`, `m_giGridBuffer`, `m_giGridAddress`, `m_giLevelTextures`, and octree-only frame bindings once all references have been migrated.
- Remove the octree-only CPU fog-GI brick source classes, globals and fields. Keep fog density field and the GI-volume fog mean layers.
- Delete the `GIVariant::Octree` selection and `GI_OCTREE` permutation after auditing all lit/mesh/filtered shader variants and specialization IDs. Only then rename `Full` to `Volumes`, updating all C++/HLSL and tests together. Do not blindly renumber shared ABI layouts.
- Delete the octree-specific `SgGiVolume` serializer, dense-level GPU packer, old baker entry points, and shader walkers **only after** confirming they are not reused by regular volumes or SG helper math. Preserve shared `SG` basis/lighting utilities.
- Clean up the remaining inspector text, comments, docs, tests and examples; remove misleading wording that local GI volumes need an octree underneath them.

## Validation checklist

- `git apply --check tourmaline_retire_octree_runtime.patch` succeeds against the matched repository revision, then apply it.
- Build the `App` and editor targets; build shader permutations including `GI_FULL` with no octree data.
- A scene with **only** `.givolume` lighting still shows baked GI on static surfaces (no `.sggi`).
- A scene with **no** GI volumes displays the proper sky or regular-volume fallback rather than stale octree data.
- Switching scenes cannot resurrect an octree pointer/buffer; `frame.giEnabled == 0` on both frames.
- World Map (C) shows volume lattices and honest counts. Note: these are lattice sites, not proof every sample is valid or inside the convex hull.
- `TOURMALINE_GI_VOLUMES=0` deliberately drops convex volume GI to regular/sky fallback; no invisible octree fallback.
- With `TOURMALINE_WORLD_FIELDS=1`, fog density streams, world-field GI mean/coverage are absent, and volume-based fog shading/sky fallback behave as expected.
- Rebuild a clean pak without `.sggi` and verify the cooker, pak registry, and runtime all accept it.
- Capture A/B screenshots of interiors, transition/fade edges, open terrain and volumetrics before merging stage 3.

## Apply stage 1

From the repository root on an **unmodified matching checkout**:

```sh
git switch -c retire-legacy-sggi
git apply --check tourmaline_retire_octree_runtime.patch
git apply tourmaline_retire_octree_runtime.patch
git diff --check
```

If you have applied an earlier World Map probes patch, do not layer this patch on top unchanged; revert that patch or rebase the overlapping map hunks first. This patch supersedes the earlier map-only fix.

**Status:** Patch is syntax/format-checked using a synthetic source fixture rebuilt from the public GitHub snippets. No live compilation, runtime lighting validation, or `git apply --check` against a full repository checkout was possible in this environment. Run the checks above on your checkout before merging.
