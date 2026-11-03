# Direct lightmap vertex pulling is opt-in

## Ordinary nonlightmapped direct draws

The original `default.vert.hlsl` compiles with the constant
no-lightmap sentinel. There is no chart-buffer binding, fetch,
decode or runtime branch in the *normal direct vertex shader*.
The direct draw path does not push extra chart metadata for those
draws. Skinned meshes never choose the lightmap variant.

## Baked static direct draws

Only a resident static selected LOD with baked SG charts opts into
`default_lightmap.vert.hlsl` (which defines DIRECT_LIGHTMAP_PASS
and includes the same base direct vertex shader). The renderer
pairs that vertex module with the existing fragment shader,
including normal opaque/ProjectLit/A2C and ordinary transparent
lit families; material shading behavior is otherwise unchanged.

- Byte 192..208: chart transform pointer/base (per selected LOD).
- Byte 224..240: optional stream offset/lobe class/magic; overlaps
  only DepthSkinTail's padding and is pushed after it.
- The variant cache is empty until an eligible baked direct draw.
- Far vertex-lit and special quality transparent routes remain on
  their existing shader programs, without baked lightmaps.
- Wireframe deliberately uses the ordinary direct vertex stage.

## Performance considerations

Ordinary unbaked direct geometry incurs no extra GPU vertex
instructions, no optional attribute memory traffic, no new
push commands and no lightmap-specific PSO selection.

There remains a cheap CPU eligibility check per direct draw and a
separate shader module to load; baked direct draws pay for their
own vertex fetches, push commands and additional PSO variants.

The global 64MiB optional lightmap heap/binding is introduced
by upstream PR #52, irrespective of this change.

## Validation required

Not compiled or GPU tested by the assistant. Before merge:
- Compare ordinary direct SPIR-V against master: no chart reads.
- Ensure wrapper compiles under DXC with matching VS outputs.
- Test mixed baked static / skinned scenes with equal materials.
- Test F10 UV1, A2C, ProjectLit, transparent, LODs, shader reload.
- Test negative delta, atlas scene reload, missing shader module.
- Benchmark nonlightmapped scenes for unchanged GPU main-pass time.
