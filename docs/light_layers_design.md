# Light layers / light linking: design and rollout

## Goal

Allow a light to affect only selected renderable receivers without
duplicating geometry or requiring separate view layers. The user-facing
model is a 32-bit **light channel mask** on each light and each renderable.
A direct-light contribution is evaluated iff
`(light.layers & receiver.layers) != 0`.

This document and `Engine/Renderer/light_layers.h` define the CPU contract
only. **This PR does not yet enable light linking in rendered frames.**
GPU integration, scene persistence, and editor controls are follow-ups.

## Compatibility and semantics

- Defaults: **all 32 bits enabled** for both lights and receivers, so
  unauthored assets render as before. The CPU helper exports
  `kAllLayers`, `kNoLayers`, `kDefaultLayer`, `Layer(bit)` and
  `Affects(light, receiver)`.
- Zero explicitly receives no linked direct light; it is not a wildcard.
  Multiple channels are ORed and overlap is sufficient (not equality).
- Layers affect **direct** point, spot, and directional illumination.
  They do **not** implicitly modify shadow caster visibility, baked GI,
  sky IBL, lightmaps, reflection probes, decals, or volumetric fog.
  Those paths need separate semantics before adding filtering.
- The mask is independent of shadow request flags, culling tile masks,
  the editor's visibility/isolation flags, and physical material layers.
- 32 channels, zero-based index 0..31. Bit 31 must work as unsigned.

## Proposed implementation sequence

1. **Scene and authoring:** Add `uint32_t layers = 0xffffffffu` to
   `SceneLight` and an explicit renderable-node receiving mask (not a
   shared mesh/material property, because two instances may receive
   different lights). Expose per-object and per-light 32-checkbox or
   compact hexadecimal mask controls. Add `.tour` verbs and replay
   validation, then round-trip source -> cooker -> cooked schema ->
   runtime. Missing fields in old content default to all bits.
2. **CPU gather and GPU layout:** Forward+ lights currently use a
   tightly matched **96-byte** `ForwardPlusLight` struct in
   `engine/include/Engine/Rendergraph/forwardplus.h` and
   `engine/shaders/common.hlsli`. Do not repurpose the shadow
   `flags` bit or the float padding. Introduce an explicit uint mask
   field in a coordinated CPU/HLSL layout revision with offset and
   sizeof assertions, and update upload strides and any consumers of
   the raw record. The directional-light path needs the same contract.
3. **Receiver to shader:** Deliver a per-instance receiving mask through
   the correct static, GPU-driven, skinned, and transparent draw paths.
   Do not assign the mask on a mesh asset or material (instances can
   differ). Propagate it to the fragment shader without interpolation.
4. **Shading:** Check mask overlap before point/spot light evaluation
   and before directional light contribution in the direct-light path.
   Never remove lights from the global Forward+ tiled lists based on a
   particular receiver: a tile can contain receivers on different
   channels. Keep reflection-probe entries exempt.
5. **Test and performance:** Verify default-scene pixel parity;
   overlapping and disjoint masks on two instances of one mesh;
   shadowed and unshadowed point/spot cases; directional-only case;
   transparent and skinned geometry; GPU-driven vs direct draws; bit 31;
   zero and all mask; imported and cooked `.tour` round trips.
   Confirm no per-pixel mask-specific tile rebuilds, and benchmark
   masked vs all-layers scenes.

## Risk register

**Layout and shader mismatch:** The HLSL Forward+ record is hardcoded
to 96 bytes. Any new member must land with corresponding CPU offset,
HLSL offset, upload bounds, and assertions in one implementation change.

**Shadow semantics:** A light being unlinked from a receiver must not
accidentally disable another light's shadows. Receiver-specific shadow
filtering is out of scope until the same semantic model exists for
casters and receivers.

**Backward compatibility:** Never reinterpret existing flags as layers.
Missing serialized fields mean all channels, not zero.

## CPU regression test

`c++ -std=c++20 -Iengine/include tests/test_light_layers.cpp -o /tmp/test_light_layers && /tmp/test_light_layers`

The CPU-only test guards default parity, multi-bit overlap, empty masks,
and bit 31. GPU tests will be required in the implementation PR.
