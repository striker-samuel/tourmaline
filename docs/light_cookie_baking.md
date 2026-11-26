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
`lightCookieBake` lines are unaffected.

## The geometric baker (implemented)

`tour::gi::BakeSpotLightCookie(World&, const LightCookieBake&,
LightCookieBakeResult&, std::string&)` (Engine/GI/light_cookie_bake.*) turns
a recipe into an opaque-coverage RGBA image, no GPU required:

1. Resolve the light node by stable UUID, read its `SceneLight` (spot only)
   and world frame (position = the node's global translation, aim =
   `globalTransform * (0,0,-1)` via `GltfPunctualAimAxis`, matching the
   renderer's spot build).
2. Assemble world-space triangles from **only** the recipe's occluder UUIDs
   (the source node transform is applied once, as in `AssembleTraceScene`),
   and build an occluder-only `tour::gi::TriangleBvh`.
3. Cast `samples` rays per texel from jittered emitter positions (a uniform
   disc of radius `radius`) through each cone-projected pixel to
   `distance`, averaging the unoccluded fraction in linear space — an opaque
   Embree `AnyHit` pass. White = unoccluded, black = blocked. The cone basis
   is the runtime `SpotConeUV` upRef/s/u branch, so the image V axis lines up
   with the shader's gobo projection without a flip.
4. A stale/missing light or occluder UUID, a non-spot light, or an empty
   occluder set fails loudly (never a silently-partial cookie).

The pixel payload is the CALLER's job: the `.texture` descriptor + its source
image are written through the shared `smvk::assets` texture library (the
editor / cook link source_import; the engine does not), then imported into the
cluster pool so `FindTextureByUri(cookieUri)` resolves it — the same lookup
the renderer performs for `setLightAttr cookie`. `tests/test_light_cookie.cpp`
proves the chain end to end (recipe -> bake -> .texture -> pool -> uri).

## Remaining before the in-editor workflow is complete

1. In the spot-light inspector, capture multi-selected mesh UUIDs and offer
   edit fields for the record plus a **Bake Cookie** action that writes the
   `.texture`/source image and binds the cookie URI (the editor UI was
   intentionally deferred; the underlying bake + asset pipeline is done).
2. Re-baking, cancellation, progress, source-hash invalidation, undo/redo,
   and deletion/rename behavior around the editor action.
3. Point lights need a six-face cubemap cookie **renderer and shader** path:
   Tourmaline's current cookie sampling is spot-only, so don't present point
   light cookies as supported until that is implemented.
4. The geometric baker handles opaque coverage; colored transmission,
   reflections and caustics would require a material-aware transport
   extension, beyond a binary Embree `AnyHit` pass.
