# GPU Scene closeup dither: camera-relative distance

The GPU Scene indexed raster VS and task/mesh-stage vertex output both
already propagate the subtree-inherited `InstanceFlagDitherCloseup` flag
at location 8. The masked PSO classification is also already published:
the `kDitherCloseupFlags` mask (`InstanceFlagDitherCloseup |
InstanceFlagMasked`) routes an opaque dithered object through the masked
pipeline — assemble a full instance flag word with
`MakeInstanceFlags(baseFlags, kDitherCloseupFlags)`. This change fixes a
separate large-world precision error; it does not create a second
instance flag or a dither system.

The prior path reconstructed `worldPos = relative + origin.high +
origin.low` in float32 and computed `distance(worldPos, cameraPos)` in
the fragment shader. At a camera origin around 1e12 units, a 0.5..3m
transition collapses to either zero or a huge quantized jump.

## Updated shader contract

- **Indexed VS and meshlet MS:** keep location 2 `worldPos` unchanged
  for existing lighting and material lookups. Emit a separate interpolated
  location 11 `cameraRelativePos`, calculated directly from
  `DoubleFloatRelative(instance.worldPosition, view.origin)` plus the
  transformed local vertex. `RasterViewPublisher` sets the origin to the
  camera. Do not add the high/low absolute origin to this varying.
- **Shared helpers:** `CloseupDitherFactorCameraRelative` uses the same
  0.5m hidden/3.0m solid thresholds and the same `DitherKeep` Bayer
  pattern as the existing closeup coverage; only the distance input
  changes.
- **GPU Scene masked depth/G-buffer:** dedicated permutations select the
  camera-relative varying. Legacy shared shader variants continue to use
  their existing absolute-world interface.
- **GPU Scene masked A2C lit:** selects the same relative factor so
  coverage matches its GPU Scene masked prepass.

The alpha texture cutout and the fade remain separate tests. Shader
locations 0..10 are unchanged; location 11 is additional, and the
shared non-GPU-scene fragment ABI is unchanged.

## Coverage and remaining integration

The `GpuSceneValidationTests` CPU reference checks the exact closeup
distance band at a 1e12-unit camera, including hidden, partial and solid
samples. Full DXC compilation, Vulkan pipeline-interface validation and
pixel parity still require CI/device execution.

**Scope:** the standard non-A2C GPU Scene Main lit variant still uses a
shared gfxmesh lit fragment shader with absolute float32 world-position
dither. It must acquire a dedicated GPU Scene-specific relative-distance
permutation/binding before this fix can claim full Main/prepass precision
parity at very large origins. The full lighting path also retains other
float32 absolute-world calculations. No production ownership or unrelated
rendering code is changed by this focused PR.
