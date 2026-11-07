# Light linking channels

Tourmaline supports **32 independent direct-light channels** for each light
and each renderable node. A light contributes to a receiver only when:

```cpp
(light.layerMask & node.lightLayerMask) != 0
```

The defaults are `0xffffffff` (all channels), and `0` intentionally
receives/emits no linked direct light. Channels may be combined by ORing bits;
there is no hierarchy or implicit exclusive ownership.

## Editor

Select any node and edit **Receive light channels** (8-digit hexadecimal).
Select a light and edit **Emit light channels**. Both controls are undoable
through the existing `SetLightAttr` edit command and persist through
`.tour` saves. Different nodes referencing the same mesh can use different
receive masks. Example: emit `00000002`, receive `00000002` to link only
channel 1 (zero-based); receive `00000001` to exclude that light.

## .tour authoring

Receiver masks use the existing generic target addressing (including
`-uuid` for stable node targeting). Masks are **unsigned 32-bit decimal**
integers on the wire; the editor presents them in hex.

```text
setLightAttr -n 12 -at layerMask -v 2;
setLightAttr -n 27 -at receiverMask -v 2;
setLightAttr -n 28 -at receiverMask -v 1;
```

`layerMask` requires a light; `receiverMask` works on any node.
The writer prints an exact decimal integer, not a float in reduced precision,
so bit 31 round-trips intact. The scene cooker stores receiver masks on
**dense node slots** and stores light masks in the cooked light definitions.
This updates the cooked blob layout: recook content built before this change.

## Game API

For game-created node handles:

```cpp
const auto instance = world.AddNode();
world[instance].SetLightLayerMask(1u << 3);
```

For authored world nodes, access `scene.metadata[node].lightLayerMask`
after resolving the node ID (or issue a `.tour` attribute override).

## Renderer

- Point and spot lights publish `SceneLight::layerMask` as
  `ForwardPlusLight::layerMask` at **byte 76**. This reuses the existing
  4-byte padding field, retaining the 96-byte CPU/HLSL ABI and tile-culling
  resource strides.
- The single directional sun uses the existing 32-bit `Light::flags`
  field as a direct-light mask; the default is all bits set.
- Receivers are per-**node instance**, not per mesh or per material.
  Direct indexed draws push the mask at **push-constant byte 248**;
  filtered/GPU-driven draws publish it at **GfxDraw.misc3.y**. Vertex and
  mesh shaders forward it `nointerpolation` to fragment location **10**.
- The lit fragment shader rejects disjoint point/spot lights before BRDF
  evaluation. Directional direct radiance is zeroed when the mask is disjoint.
  Existing Forward+ tiled light lists are global per tile and are **not**
  culled by a particular receiver; different receivers can share a tile.

## Intentional boundaries

Light linking affects **direct** diffuse/specular illumination only.
Sky/ambient, baked GI, reflection probes, local and directional shadow
**caster** visibility, fog, decals, and emissive are not linked channels.
A light excluded from a receiver contributes no direct term on that receiver,
regardless of its computed shadow visibility. Shadow casters still populate
the shared shadow atlases, so receivers of other lights are unaffected.

## Validation

`LightLayersTests` (CPU masks, including zero/all/high-bit) is registered
with CTest; `TourTests` covers .tour round-trips and replay of both light
and non-light receiver masks. The repository's standard CMake/CTest build and
shader compilation are the integration gates. A rendered GPU comparison of
independently masked instances is still recommended before merging.
