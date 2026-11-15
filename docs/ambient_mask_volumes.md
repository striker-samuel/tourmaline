# Ambient-mask volumes — first CPU/reference slice

This change adds a directional sky-visibility payload to Tourmaline's
existing shared `SpatialVolumeBox` OBB geometry. It does **not** implement
the complete RDR2-style AMV runtime or claim that Rockstar's exact field
format is known.

## Representation

- Six visibility factors per grid sample, ordered local +X,-X,+Y,-Y,+Z,-Z.
- A tightly packed X-major 3D lattice; trilinear spatial interpolation.
- A normalized, signed-axis directional interpolation.
- The *existing* SpatialVolumeWeight manages soft per-face blending.
- A missing/invalid field returns 1 (unoccluded). Overlaps use minimum
  visibility instead of multiplicative darkening.
- Only the **ambient sky fallback** should be modulated. Baked SG GI,
  local direct lighting, and screen-space AO must not be attenuated again
  unless their energy/visibility decomposition is explicitly defined.

## Next integration steps (not implemented here)

1. Author/cook a volume association with a 3D visibility payload and
   a versioned, validated tile/page format.
2. Stream pages using WorldPartition interests and StreamBudgetPool credits,
   publishing GPU descriptors only after upload completion.
3. Add a shader sample path for the ambient-sky fallback, with debug view.
4. Provide an offline static-sky-visibility baker; test occluders, interior
   transitions, rotated volumes, overlap, streaming churn and time-of-day.
5. Profile storage and bandwidth. Six float samples per point in this
   reference path are intentionally *not* a production GPU encoding.

## Reference contract

`engine/include/Engine/Scene/ambient_mask_volume.h` is CPU-only and
reuses `spatial_volume.h`. The grid is not a runtime scene component or
a texture binding yet. The caller owns the memory backing the span.
