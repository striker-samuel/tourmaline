# Geometry capture atlas for asset reverse search

`tools/geometry_capture_atlas.py` is an offline, CPU-only processor for the
engine's imported **MESH v3** `.geometry` products. It does not parse glTF,
reimport source assets, or require the Vulkan renderer. Install its one optional
dependency with `python -m pip install pillow`.

```sh
python tools/geometry_capture_atlas.py content/models \
    --output out/geometry-atlas --face-size 128 --atlas-width 2048 \
    --padding 8 --mips 8
python -m unittest discover -s tools/tests -p 'test_geometry_capture_atlas.py' -v
```

One or more `.geometry` files or directories are accepted; directory inputs are
recursive and sorted by canonical path for deterministic packing. The processor
reads the packed position, color, half-float UV, octahedral normal and tangent
streams, surface offsets and node transforms. It resolves each instance's
material GUID against neighboring `.material` documents and samples referenced
PNG/JPEG/etc. images and `.texture` source aliases (nearest-neighbor,
repeat/mirror/clamp). Missing materials or images **fail** rather than producing
misleading signatures. KTX2 sources currently require an explicit conversion
to a Pillow-readable image; no GPU decoder or runtime texture transcoder is
included. The base mesh is captured, not LOD siblings or animated skin poses.

## Captures and products

Each asset has six orthographic captures, one looking along each signed local
axis (`+X/-X/+Y/-Y/+Z/-Z`). The three axes form a triplanar representation,
while both signs preserve front/back silhouettes for retrieval. The camera is
centered on the transformed geometry's AABB, with a common maximum-axis
scale for all views. CPU triangles use depth testing and UV/material interpolation.
This is a reference **geometry/material capture**, not a lit scene render:
lighting, cast shadows, translucency compositing, and project shader hooks do
not participate.

Each level writes six separate PNG atlases with identical rectangle packing:

| File | Channel contract |
| --- | --- |
| `albedo_mipN.png` | sRGB-style RGBA base color × vertex color, alpha=coverage |
| `normal_mipN.png` | World-space normal remapped from [-1,1] to [0,255] |
| `orm_mipN.png` | Occlusion / roughness / metallic / alpha |
| `emissive_mipN.png` | Emissive RGB / alpha |
| `coverage_mipN.png` | White RGB / alpha visibility |
| `material_id_mipN.png` | Three-byte local material ID (0 = background) / alpha |

For masked materials, pixels below the alpha cutoff are discarded. Blended
materials retain alpha in the capture (they are *not* depth-sorted or
composited). Each view is packed as a padded tile; the border repeats the
tile's edge pixels to limit filtering bleed. The same layout is shared
across all channels. Lower mip levels are produced per tile **before packing**
(box-filtered channels, nearest material IDs), so neighboring assets do not
bleed into one another. Coverage/normal/ORM mip averaging is a compact
search representation, not a physically normalized render texture.

## Reverse lookup manifest

`index.json` lists every mip's image dimensions and channel filenames,
then every source asset's SHA-256, optional geometry GUID from its nearby
`*.gltf.meta` (or other mesh-source manifest), world-space bounds and size,
triangle count, material GUID → local ID mapping, and the pixel-space
`rect` and `views` rectangles for **every mip**. Rectangle coordinates
have top-left origin, in pixels; the dimensions are explicit and may differ
between levels. A reverse-search backend can crop a chosen view and channel
at the desired mip, compare coverage/material features, and map matches
to stable geometry/material identities in this manifest. Build the ANN index,
descriptors, or similarity scoring in a downstream consumer; this command
only generates the capture corpus.

The current format is a file-per-channel, file-per-mip set of PNGs plus JSON,
not a single GPU KTX2 texture with internal mip levels. The `--mips 0` default
produces the full per-tile chain down to 1×1.
