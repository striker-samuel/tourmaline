# GPU-scene packed skin work (staging contract)

## Scope

This change adds a device-independent, frame-compact work-list contract for
future GPU-scene skinning. **It does not admit skinned meshes to GPU-scene
raster, launch a new GPU compute pass, or replace the legacy skinning path.**
Legacy ownership is preserved until compute, draw fetches and animated bounds
are integrated and parity-tested.

Unlike the current Northlight global `deformedBase` span, every work row
contains its own input and output vertex bases. Several independently posed
instances of the same mesh can share bind-pose vertices and influence streams
while writing disjoint intervals into a *single* deformed output buffer.

## Producer contract

`PackSkinWork` accepts one source for each independently posed instance.
Rows retain the stable GPU-scene instance slot **and generation** for later
generation-checked consumers. Prefix packing assigns contiguous output vertex
bases in input order. Five 32-bit words per output vertex match the existing
Northlight deformed stream: three position words, normal, tangent.

Producer validation is intentionally fail-closed:

- Every source must have a valid instance slot/generation; no duplicate slots.
- Skin stream length must equal the full vertex count. No silent clamping.
- Palette must have 1–256 joints and valid 32-bit offset/count arithmetic.
- Source positions/influence spans must not overflow 32-bit addressing.
- The total output must fit the **real allocated** output capacity.
- A bad batch must leave the previously published CPU output untouched. The
  caller must keep all affected instances on the legacy path.

The shader mirror in `gpu_scene_skinning.hlsli` defines the 48-byte row and
provides a bounded helper that converts a source vertex ID into a compact
deformed word offset. It is not yet referenced from an active shader.

## Next integration PRs

1. Publish the work table and a per-instance binding lookup on acquired
   flight slots, with generation validation and flight-fenced retirement.
2. Extend the skinning compute pass to read per-work palette offsets and
   write compact output spans; keep bind-pose positions/influences shared.
3. Bind the compact output to GPU-scene **indexed raster** and depth/G-buffer
   variants, with a per-draw checked local vertex address. Existing static
   PSO bins can remain unchanged.
4. Build conservative animated bounds before GPU culling, integrate motion
   vector history, and test static/skinned parity across in-flight frames.
5. Enable GPU-scene ownership only after the full path is verified; mesh-shader
   skinning and skinned simplification/LOD support are separate follow-ups.

## Failure signatures to test before admission

The historic November 8 fixes addressed stretching triangles caused by an
incorrect selected-LOD skinned flag and by short influence streams. New work
must test a missing skin binding, wrong generation, selected LOD mismatch,
out-of-range `baseVertex`, capacity overflow, pose sharing and multi-flight
reuse. No failure may result in a partially deformed draw.
