# Fixture light cookie baking (design and record format)

The fixture workflow follows Pierre Yves Donzallaz, *Create High-Quality Light
Fixtures in Unity* (2019), especially pp. 9–16 (capture sphere, point-emitter
radius and selective self-shadowing) and pp. 19–24 (capture isolation and import).

## Current implementation

A `.tour` can retain the inputs of a fixture-cookie bake independently of
the generated texture:

```tour
lightCookieBake -light "spot-uuid" -output "content/textures/wall_lamp.texture" -resolution 512 -samples 128 -radius 0.025 -distance 3 -occluder "shade-uuid" -occluder "reflector-uuid";
```

The parsed `tour::LightCookieBake` records are retained by the editor's
save/replay mirror. The output is a project-relative `.texture` asset. The
standard `setLightAttr cookie` binding remains the separate reference to
the actual baked texture (the recipe is not sampled by the renderer).

Fields:
- `light`: UUID of the source light, not its mutable node index.
- `occluder`: repeatable UUIDs of **only** meshes participating in
  self-occlusion. Excluding bulb mounts/large arms avoids projecting
  irrelevant shadows back onto the fixture itself (guide p. 13).
- `output`: project-relative `.texture` asset target.
- `resolution`: power-of-two texture width/height (16–4096).
- `samples`: rays per output texel (1–4096).
- `radius`: source radius in meters; 0 gives hard shadows.
- `distance`: reference capture radius in meters; affects penumbra width.

Malformed records are rejected on load. Existing tours without
`lightCookieBake` lines are unaffected. This initial PR is **the persistent
recipe foundation only**; it does not yet trace rays or write image pixels.

## Remaining implementation before the workflow is functional

1. In the spot-light inspector, capture multi-selected mesh UUIDs and offer
   edit fields for the record plus a **Bake Cookie** action. Validate missing
   lights/occluders without discarding their persisted authoring records.
2. Assemble world-space triangles from selected occluders only, using the
   existing GI triangle-soup extraction / `tour::gi::TriangleBvh` Embree backend.
   Source shape transforms must be applied exactly once.
3. For spot lights, cast rays from jittered emitter positions through each
   cone-projected pixel to the reference receiver sphere; average unoccluded
   fractions in *linear* space, with white meaning unoccluded and black
   meaning blocked. Align UV orientation with the spot-cookie shader.
4. Write a texture source image and a `.texture` descriptor through the
   existing texture asset pipeline; import and set the light's cookie URI
   only after successful atomic output. Preserve previous assets on failure.
5. Support re-baking, cancellation, progress, source-hash invalidation,
   undo/redo, and deletion/rename behavior. Add a fixture integration test
   proving shadows persist after editor save/reload and cook.
6. Point lights need a six-face cubemap cookie **renderer and shader** path:
   Tourmaline's current cookie sampling is spot-only, so don't present point
   light cookies as supported until that is implemented.
7. The initial geometric baker can handle opaque coverage; colored
   transmission, reflections and caustics would require a material-aware
   transport extension, beyond a binary Embree `AnyHit` pass.
