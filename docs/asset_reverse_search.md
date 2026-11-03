# Asset reverse search and image fabbing CLI

This is a **stacked follow-up to PR #89** (`geometry_capture_atlas.py`).
It uses that tool's `index.json` and mipmapped material-channel PNG
atlases. It never mutates `.geometry`, `.material`, cooked paks, or asset
catalog IDs. Only Pillow is needed for local operations:

```sh
python -m pip install pillow

# 1. Bake six signed-axis material captures (from PR #89).
python tools/geometry_capture_atlas.py content/models \
    --output out/capture --face-size 128 --atlas-width 2048 --padding 8

# 2. Turn capture rects into persistent, compact descriptors.
python tools/asset_reverse_search.py index \
    --atlas out/capture --output out/capture/search.json --grid 8

# 3. Find assets physically and visually similar to a selected geometry.
python tools/asset_reverse_search.py search \
    --index out/capture/search.json \
    --geometry content/models/tree/tree.geometry \
    --size-ratio 1.5 --limit 20 --json

# 4. Search from an existing indexed mesh, name, GUID or digest.
python tools/asset_reverse_search.py search \
    --index out/capture/search.json --asset tree.geometry \
    --weights shape=0.4,color=0.15,size=0.25 --limit 10

# 5. Search from an image; specify which capture direction it represents.
python tools/asset_reverse_search.py search \
    --index out/capture/search.json --image reference.png \
    --view +Z --limit 10 --json

# 6. Deterministic, measured asset description and image prompt.
python tools/asset_reverse_search.py describe \
    --index out/capture/search.json --asset tree.geometry

# 7. Prepare an offline fabrication plan (default: NO API call or cost).
python tools/asset_reverse_search.py fab \
    --index out/capture/search.json --asset tree.geometry \
    --style "stylized game prop" --output out/fab/tree

# 8. Explicit opt-in paid image request, with optional image reverse search.
#    Requires an API key in the environment; the key is never written to disk.
OPENAI_API_KEY=... python tools/asset_reverse_search.py fab \
    --index out/capture/search.json --asset tree.geometry \
    --output out/fab/tree --execute --search-results 5
```

## What is indexed?

The `index` command reads **actual per-view pixel rectangles** at the chosen
capture mip, extracts a small spatial grid from each axis view and channel,
and writes a standalone `search.json`. Descriptors are base64-packed RGBA8
material-channel samples, together with geometry GUID, bounds/size,
material GUIDs, hash and source path. No GPU, external database or remote API
is needed to build or search the index. The default mip is the one closest to
32 pixels per view; `--mip N` forces a particular capture level and
`--grid N` chooses 2–32 spatial samples per axis.

The `search` command accepts:

- `--asset` for indexed path, unique filename, geometry GUID or SHA-256;
- `--geometry` (alias `--mesh`) for MESH v3 `.geometry` / binary `.mesh`
  files, or an explicitly authored text `.mesh` shim of the form
  `geometry=relative/path.geometry`. Other higher-level placeable
  `.mesh` records have no known standalone binary contract and are **not**
  silently parsed as geometry; resolve their source first.
- `--image` for a single-view image (ideally transparent PNG), with
  `--view +/-X|+/-Y|+/-Z` and optional `--dimensions X,Y,Z`.

## Score dimensions (0–100, higher is closer)

| Dimension | Method | Default weight |
| --- | --- | ---: |
| shape | spatial alpha/coverage soft intersection-over-union across views | 0.30 |
| color | coverage-weighted base-color RGB difference | 0.20 |
| normal | coverage-weighted decoded world-normal cosine | 0.10 |
| roughness | ORM green-channel difference | 0.10 |
| metallic | ORM blue-channel difference | 0.05 |
| occlusion | ORM red-channel difference | 0.05 |
| emissive | emissive RGB difference | 0.05 |
| size | 2^-mean(|log2(per-axis-size-ratio)|) | 0.15 |

The overall score is the weighted mean of **available** dimension
similarities, expressed on a 0–100 scale. It is a heuristic ranking,
**not** a learned probability or a calibrated percent confidence.
Weights are customizable via `--weights dimension=value,...`.
A `--size-ratio 1.5` filter keeps only candidates whose X/Y/Z dimensions
each differ by at most 1.5×; `0` disables this hard filter. The default is
2.0. Flat meshes use a small scale-relative epsilon for size comparisons.

For a single-image query there are no captured normal/ORM/emissive channels
and no physical-size evidence unless `--dimensions` is provided. Unavailable
dimensions are marked `null` and removed from the weighted denominator.
Opaque images are treated as covering the full frame, so transparent,
centered prop renders produce more useful silhouettes. No generative model
can infer actual 3D dimensions from an image alone here.

By default a source is not returned as its own result. `--include-self`
overrides that for debugging. The matcher performs an **exact linear scan**
of in-memory descriptors, not an approximate nearest-neighbor search.

## Image fabbing

`describe` builds a reproducible description from the indexed asset's
size, broad proportions, base-color average, roughness, metallicness, and
material count. It does not invent a specific semantic class or pretend to
recover details not represented in the capture. `fab` turns this description
into a prompt. Without `--execute`, it only writes `fab.json` and costs
nothing. With `--execute`, it calls the OpenAI Images generation API with
`OPENAI_API_KEY`, writes `fab_NNN.png` and per-image SHA-256s, and
optionally reverse-searches each generated image with `--search-results N`.
It defaults to `gpt-image-2.5-flare`, medium quality, 1024×1024 PNG and a
transparent background. Use `--model`, `--quality`, `--size`, `--count`
and `--background` to change the request.

These generated files are **2D concept images**, not reconstructed 3D
geometry or ready-to-import mesh artifacts. Image generation requires network
access, provider credentials and may incur API charges. The offline
description, index, and similarity ranking do not. Tests mock HTTP requests
and never call a live image API.

```sh
python -m unittest discover -s tools/tests -p 'test_asset_reverse_search.py' -v
```
