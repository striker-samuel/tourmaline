# FFT bloom quality mode (experimental)

Tourmaline's default bloom remains the SPD five-level Karis pyramid and
additive tent-upsample reconstruction. Set `TOURMALINE_BLOOM_FFT=1` to enable
a **second, opt-in FFT convolution pass** over the reconstructed mip 0.
Set it to `0` or unset it to use the original algorithm.

## Implementation

- `engine/shaders/fft_bloom.cs.hlsl` computes a local 32x32 FFT in shared
  memory and outputs a 16x16 central tile (8 texels of overlap on each side).
  The two-dimensional forward and inverse transforms each consist of
  radix-2 horizontal and vertical passes.
- A normalized Gaussian-mixture PSF is applied as a real-valued frequency
  response: 65% isotropic core, 17.5% horizontal anisotropic component,
  17.5% vertical anisotropic component. The constant PSF avoids a separate
  per-frame kernel transform.
- The FFT output occupies its own sampled/storage RGBA16F image, and the
  render graph declares independent read/write access for the convolution.
  `BloomChain::MipView(0)` and `MipBindless(0)` switch to this result when
  the mode is available, so the existing tonemap composite is unchanged.
- Missing FFT shader modules or an unavailable FFT target fall back to the
  original bloom without disabling the effect.

This is deliberately a **tiled, finite-support frequency-domain filter**,
not a full-image FFT. The chosen halo truncates very distant PSF tails.
It is intended as a first quality mode for lens-shaped local glow, not as a
replacement for truly screen-spanning diffraction spikes.

## Validation checklist

This PR has not been compiled or GPU-validated in the authoring environment.

1. Build the shader pack and verify `fft_bloom.cs.hlsl.spv` is generated.
2. Capture the bloom result with `TOURMALINE_BLOOM_FFT=0` and `=1`.
3. Verify render-graph barriers / Vulkan validation for sampled mip 0 ->
   FFT storage destination -> sampled tonemap input.
4. Inspect half-resolution odd extents, resize, very small targets,
   and bright features at tile boundaries for seams.
5. Check visual parity with a CPU FFT reference (impulse, flat field,
   thin highlights); profile dispatch duration at 1080p and 4K.
6. Test hot reload and missing-shader fallback.

The default remains unchanged until this quality mode is explicitly enabled.
