# Composite lens flares

The HDR tonemap composite now adds three screen-space optical ghosts and a
radial halo using the existing bloom bright-pass texture. The contribution is
added in linear HDR before exposure and tonemapping; no new render pass,
render target, readback, or scene-light list is required.

- `TonemapDesc::lensFlareIntensity` defaults to 0.15; set it to 0 to disable.
- The effect requires bloom to be enabled and available. Dark scenes without
  thresholded highlights have no flare contribution.
- The optical LUT is an immutable linear RGBA8 texture uploaded once alongside
  shader setup. If allocation fails, the pass uses its existing bloom/tonemap
  path. The ghost taps are clamped to their layer's source UV window and
  smoothly fade at its edges.
- Motion, clipping debug and display calibration views skip the optical effect.
- This is a screen-space approximation, not a physical camera simulation:
  off-screen lights and occluded light sources are not independently tracked.

## Texture license

The original two-row optical lookup artwork (spectral tint and aperture halo
profiles) in `engine/include/Engine/Rendergraph/lens_flare_tex.h` is dedicated
under [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/).
No third-party artwork is incorporated. The engine and shader code retain
their existing MIT license.

## Manual validation checklist

- Compile the tonemap vertex and fragment shaders and check that the
  C++/HLSL push layout is 128 bytes.
- Compare a scene with one small HDR emitter near a viewport corner with
  intensity 0 and 0.15. Verify reflected ghosts/halo appear opposite it.
- Disable bloom, remove the emitter, or set the flare intensity to zero:
  no flare contribution should be visible.
- Exercise raster and FFT bloom, a dark scene, ultrawide and portrait output,
  SMAA and a stacked/sub-rect viewport; check edge isolation and finite output.
- Check motion/clipping debug and the display calibration pattern; the flare
  must not modify those views.
- Check shader hot reload and shutdown for bindless resource lifetime issues.
