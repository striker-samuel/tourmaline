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
built from master `242ebe70ac885923af118ef4e6feae60e6e98c90` (the
releases-branch drop commit `028c7a62` records the same). On a refresh:
note the NEW master hash here and in the drop commit message so the
bake stays reproducible against a bisect.
