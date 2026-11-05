# Project recipe: material channel packing

Material packing is defined by **project-level `.tourrecipe` directives**, not by scene `.tour` files. The canonical project importer resolves the rule, writes derived textures beside the imported material assets, and stamps the resolved layout on each `.material`. The cooker then uses those material assets; shader code receives canonical texture channels and an explicit scalar selector word.

## Example

```text
recipe 1
project MyGame
entry content/scenes/main.tour

# Source slots: baseColor, normal, emissive, metallicRoughness,
# occlusion, cavity. Constant slots: one, zero.
# Target channels: baseColor.r/g/b/a, normal.r/g/b, emissive.r/g/b,
# and scalar targets ao, cavity, roughness, metallic.
materialChannel baseColor.r baseColor r
materialChannel baseColor.g baseColor g
materialChannel baseColor.b baseColor b
materialChannel baseColor.a baseColor a

materialChannel normal.r normal r
materialChannel normal.g normal g
materialChannel normal.b normal b

materialChannel emissive.r emissive r
materialChannel emissive.g emissive g
materialChannel emissive.b emissive b

materialChannel ao occlusion r
materialChannel cavity occlusion a
materialChannel roughness metallicRoughness g
materialChannel metallic metallicRoughness b

# Optional cooked KTX2 formats for generated output groups.
materialEncoding baseColor bc7
materialEncoding normal bc5
materialEncoding emissive bc7
materialEncoding scalar bc7

[pak world]
output game.pak
scene content/scenes/main.tour
include content/models/**
```

The composer uses the material's **existing source texture slots**, not arbitrary file paths. Each target may read any source slot's R/G/B/A component or an explicit constant `one`/`zero` (channel argument still required).

## Outputs

| Material group | Generated texture | Canonical output channels | Transfer |
| --- | --- | --- | --- |
| Base color / opacity | `<guid>.baseColor.packed.png` | RGBA | sRGB |
| Normal | `<guid>.normal.packed.png` | RGB (alpha=1) | Linear |
| Emissive | `<guid>.emissive.packed.png` | RGB (alpha=1) | sRGB |
| Scalar PBR | `<guid>.scalar.packed.png` | R=AO, G=cavity, B=roughness, A=metallic | Linear |

A group is generated only if at least one of its targets is explicitly specified. Unconfigured channels in that group retain the corresponding original glTF source channel or the neutral fallback. The normal shader currently consumes packed X/Y and reconstructs normal Z; the authored B channel is retained in the image but not read by the existing normal-mapping path.

`materialEncoding <baseColor|normal|emissive|scalar> <format>` sets the cooked KTX2 encoding for a generated group. Supported formats are `rgba8`, `bc3`, and `bc7` for all groups; `bc5` is also supported for normals (XY; reconstructed Z), and `bc1` for emissive. A directive requires at least one `materialChannel` in its group. The importer writes a derived `.packed.texture` settings asset pointing at the lossless PNG; the normal `.texture` transcode path encodes the KTX2. If absent, the existing RGBA8 behavior is unchanged. Changing a format changes the packing signature and invalidates import metadata. Encodings are project-wide, not per material.\n\nInput images must have identical dimensions when combined into one output. Incompatible sizes and explicitly requested missing input slots cause an import error rather than silent stretching or undefined sampling. The importer creates lossless PNGs; the existing texture loader converts them to the cooked GPU format. Each rule is checked during import and can trigger regeneration when the recipe changes.

## Metadata and compatibility

The derived `.material` stores `packing.source=compose`, a deterministic `packing.signature` describing all channel mappings, `packing.channels` (the scalar packed channel selector word), and the generated image references. The cooked GPU material's 128-byte `MaterialValues` row carries the scalar selector word via its reserved `tex2.w`. Base color, normal and emissive use their unchanged canonical runtime fetch layouts.

Materials without any packing directive remain on the glTF defaults. The older `materialPacking <metallicRoughness|occlusion> <AO> <cavity> <roughness> <metallic>` directive remains supported as a source-reuse mode; it cannot be combined with `materialChannel`.

## Verification

`tests/test_project_pipeline.cpp` contains table-driven policy cases and image-level regression tests for all four output groups, recipe syntax, metadata round-tripping, changed recipes, invalid/missing inputs, and legacy behavior. Run the project pipeline test target and shader/editor builds before merging.
