# Demo content releases (`.paks`)

Paired demo content for the repo — the playable paks built by the
canonical project pipeline. The `.pak` binaries are deliberately NOT
on `master` (gitignored): big binaries don't belong in the code flow.
They are committed on the **`releases` branch** and pushed to the
**remote named `oldpc`** only. Publishing to GitHub releases (or a
LFS move) happens later, on purpose — see "Publishing" below.

## The paks

| pak | scene | built from | how to run |
|---|---|---|---|
| `paks/sponza.pak` | the Sponza demo (the canonical project: `Project.exe init/build` → Cook → GiBaker → Pak — sun + LOD + meshlets + 131k-probe GI + 8 cubemap captures at spp 32) | the current `master` (see the commit line at the bottom of this file) | `cd out/build/x64-Release && App.exe --pak releases/paks/sponza.pak` (or copy next to App.exe and `--pak content/paks/sponza.pak`) |
| `paks/sponza.tapp` | the SAME sponza build's appmanifest (the recipe-build form: `Project.exe build --project <dir> --recipe sponza.tourrecipe` → verified pak + the generated `.tapp`, publish-last) | the current `master` (same drop as the pak) | `App.exe --project releases/paks/sponza.tapp` (put the `.tapp` + `.pak` side by side — the manifest resolves paks relative to itself; `--no-vfs` proves self-containment) |
| `paks/_probe.pak` | the GI probe-ball diagnostic scene (cubemap + SG probe validation view) | an earlier session's cook | `App.exe --pak releases/paks/_probe.pak` |

Numbered variants (`sponza_1.pak`, `sponza_2.pak`, ...) in the build
tree are incremental re-cooks of the SAME project — not separate demos;
they are not carried here.

## Bistro (recreate-from-source, not a shipped pak)

The Amazon Lumberyard Bistro exterior (CC BY 4.0 ORCA, see
`THIRD_PARTY_NOTICES.md`) cooks to a **~1.4 GB** pak and its source textures
are **~700 MB**, so neither is carried here. Instead the small *definition*
— recipe + scene + glTF + identity ledger + 254 material assets (~2 MB) —
is committed at [`releases/bistro/`](bistro/README.md). To recreate:

1. Download the ORCA
   [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)
   (FBX + textures).
2. Convert the FBX to glTF and the textures to KTX2, matching the committed
   `bistro_static.gltf` URI layout.
3. Run `tools/recreate_bistro.ps1` (Windows) / `tools/recreate_bistro.sh`
   (Linux), or the `Project.exe build --recipe bistro_static.tourrecipe`
   command in [`bistro/README.md`](bistro/README.md).

See [`bistro/README.md`](bistro/README.md) for the full flow.

## Knight (Animated Knight, skinned + animated)

The Intel **Animated Knight** (GPU Research Samples Library, CC BY 4.0, see
`THIRD_PARTY_NOTICES.md`) is a skinned, animated character demo — the GPU scene's
skinned task/mesh lane (1 skin, 85 joints) plus a walk clip (~258 tracks,
~12.5 s). The cooked pak is small (~5 MB), but the faithful Maya → glTF
conversion is non-trivial, so the small *definition* — recipe + scene tour +
glTF + identity ledger + 2 material assets (~130 KB) — is committed at
[`releases/knight/`](knight/README.md), and the authoritative `.bin` geometry
buffer ships with the release asset set rather than on `master`. See
[`knight/README.md`](knight/README.md) for the build/run flow.

## Refreshing the paks

```powershell
# The RECIPE build (the canonical path now): resolve -> import
# freshness -> Cook -> GiBaker -> Pak (verified + policed) -> the
# appmanifest publishes LAST. Needs the sponza.tourrecipe in the
# project (see F:/projects/game) + the stage exes beside Project.exe.
& .\out\build\x64-Release\Project.exe build `
  --project F:/projects/game `
  --recipe F:/projects/game/sponza.tourrecipe `
  --publish F:/projects/game/build --manifest-name sponza
# then copy the build into the release folder (pak + manifest together)
Copy-Item F:\projects\game\build\sponza.* releases\paks\
```

Bistro is NOT carried as a shipped pak — see "Bistro (recreate-from-source)" above; `tools/recreate_bistro.ps1` / `.sh` drive its cook from the ORCA source.

The legacy one-liner (`tools\cook\cook_sponza.bat`) still works — it
drives the same stages through the legacy `BuildProject` path.

The pak embeds the GI probes + cubemap captures baked at cook time —
a code change that alters bake OUTPUT (not bake reading) needs a
re-cook to refresh the embedded data; the bake-freshness gate inside
the project handles incremental re-imports automatically.

## Publishing (later, on purpose)

1. The `releases` branch carries the binaries (`git add -f` — master
   gitignores them). It is pushed to `oldpc` only:
   `git push oldpc releases`.
2. When GitHub distribution is wanted: either upload the paks as GitHub
   release assets against a tag (the recommended form — no git payload),
   or move the branch to LFS first.
3. Tag the exact commit each pak was built from (below) so a future
   bisect can reproduce the bake.

---
built from `gpu-scene/world-partition-streaming-gpu-update` `c53b3ab6`
(the SMP9 pak-format epoch: meshlet trailer carries `maxLocalVertex`).
On a refresh: note the NEW hash here and in the drop commit message so
the bake stays reproducible against a bisect.

SMP9 note: the pak format bumped (SMP8 -> SMP9) to persist the
meshlet `maxLocalVertex` at cook time. `sponza_10.tapp` +
`sponza_10-2316986f406faa97.pak` in the `demo-content` release are
SMP9; `knight.pak` is still SMP8 and needs a re-cook.
