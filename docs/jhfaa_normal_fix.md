# JHFAA-style shading-normal validity correction

Branch: `feature/jhfaa-normal-fix` · Status: **experimental, default OFF**

## The problem

JHFAA (Johan Hammes Fresnel Anti-Aliasing) targets a *source* of specular
aliasing rather than filtering its symptoms post-hoc. A rasterizer
interpolates vertex normals across each triangle:

```
Ns = normalize(sum(wi * Ni))
```

On a coarsely tessellated, smooth-shaded mesh, that interpolated shading
normal — further perturbed by normal maps — does not represent physically
valid geometry at silhouette pixels:

- `Ns` can point **away from the camera** while its triangle is still
  front-facing (`NoV < 0`).
- The reflection vector `R = reflect(-V, Ns)` can point **through the
  polygon** (below the geometric tangent plane), sampling bright,
  unrelated environment radiance that sparkles as the camera or geometry
  subpixel-shifts.

Raising roughness or damping specular intensity does not fix the
underlying *direction* — the lobe keeps pointing at the invalid sample.
Hammes has publicly described the problem and the principle, but there is
no public reference implementation of the complete normal-correction
mathematics (his detailed document was prepared for Epic and shared
privately). **This is therefore a practical correction in the same
spirit, not a reproduction.**

## The correction

`engine/shaders/shading_normal_fix.hlsli`:

1. **Face the viewer** — if `NoV < 0.001`, nudge `N` by
   `N + (eps − NoV)·V` (lands `NoV` exactly at `eps` by construction).
2. **Geometric horizon** — reflect, and if `dot(R, Ng) < 0.002` push `R`
   back above the tangent plane of the *actual rasterized triangle*:
   `Ng` comes from `cross(ddx(worldPos), ddy(worldPos))`, oriented toward
   the camera — **not** the interpolated vertex normal (the two are
   exactly what disagree at a silhouette).
3. **Reconstruct the normal** from the half-vector identity
   `N = normalize(V + R')`, which round-trips exactly
   (`reflect(-V, normalize(V+R)) == R`). One corrected normal then feeds
   every downstream consumer — sun lobe, forward+ lights, env/probe
   specular vector, GI specular crossovers — so they all agree on a
   physically plausible direction. (Simply clamping `NoV` would leave
   `R` invalid; clamping `R` alone would desync every other consumer of
   `N`.)

Tolerances (`0.001` / `0.002`) are prototype values, not published JHFAA
parameters.

### Deliberate scope limits (first experiment)

- **Opaque, single-sided-lit geometry** via the main lit family
  (`default_lit_body.hlsli` — the classic, A2C and mesh twins all share
  the body and get it automatically). The POM debug view (F11) is
  intentionally excluded; two-sided / alpha-tested edges need their own
  treatment.
- **Fresnel is untouched.** The existing GGX + Schlick evaluation already
  carries grazing-angle Fresnel; the correction must not zero the
  legitimate silhouette reflectance, so no extra multiplier is applied on
  top. A dedicated silhouette reflection sourcing (Hammes uses
  previous-frame screen-space reflections for the last few edge pixels,
  no full reprojection) is a follow-up experiment on the existing SSR
  infrastructure.
- **SSR normal consistency:** the lit path now reflects off the corrected
  normal; the SSR trace reconstructs its ray from the thin G-buffer's
  *interpolated geometric* normal (`gbuffer.frag.hlsl`, oct-encoded R/G).
  The env-spec slice SSR replaces (`outEnvSpec`, MRT target 2) is
  exported from the *corrected* reflection — consistent on the composite
  side — but a fully consistent trace would also correct the G-buffer
  writer's normal. That is the documented follow-up once the direct +
  probe path is validated.

## Gating + A/B contract

- `TOURMALINE_NORMAL_FIX=1|on|0|off` — pins the correction headless
  (default **off** while under evaluation; validated by
  `tests/test_runconfig.cpp`).
- `TOURMALINE_NORMAL_FIX_DEBUG=1` — the red/orange violation overlay:
  **red** = back-facing shading normal (`NoV < 0`), **orange** =
  reflection below the geometric horizon (`dot(R, Ng) < 0`), black =
  clean. Keys off the **un-corrected** normal, so it works with the
  correction off (baseline extent) and on (residue).

Both ride `FrameUniforms::flags` (`kFrameFlagNormalFix` = bit 1,
`kFrameFlagNormalFixDebug` = bit 2; HLSL mirror `FRAME_FLAG_NORMAL_FIX*`
in `common.hlsli`) — no struct layout change, the append-only ABI is
untouched.

Placement in the lit body: after decals + normal mapping (the final
shading normal), before every consumer (`NdotV`,
`SpecularAARoughness`, the sun lobe, the forward+ lights,
`R = reflect(-V, N)`, the GI specular crossovers). Screen-space
derivative reads follow the existing `SpecularAARoughness` precedent
(uniform-flow reads after the alpha-test discards; helper invocations
keep quad derivatives defined).

## Known limitations / watch list

- The helper quad at a silhouette spans the neighboring triangle too, so
  the horizon plane is approximate for the 1-pixel edge band.
- Near-dot pixels (`|dot(Ng, V)| ~ 0`) have ambiguous face orientation;
  the horizon constraint is weak there anyway (the plane is nearly
  edge-on), so the sign flip is harmless in practice.
- The corrected normal has a kink *at* the correction threshold —
  `SpecularAARoughness` variance spikes there; its kernel clamp (0.20)
  bounds the roughness bump.
- Watch for regressions on: concave surfaces, mirrored-UV seams, heavily
  normal-mapped surfaces, very coarse tessellation (the correction can
  visibly change shading there — it should be *evaluated*, not assumed
  better).

## Test matrix (headless capture workflow)

Same scene, camera and bench budget; vary only the pins:

| Run | Pins | What to look for |
| --- | --- | --- |
| Baseline | `TOURMALINE_AA=off` | Sparkling / invalid reflections at silhouettes |
| Corrected | `TOURMALINE_NORMAL_FIX=1 TOURMALINE_AA=off` | Reduced artifacts, no shading discontinuities |
| Corrected + SMAA | `TOURMALINE_NORMAL_FIX=1` + SMAA | Whether SMAA still adds useful edge coverage |
| Corrected + SSR | `TOURMALINE_NORMAL_FIX=1 TOURMALINE_SSR=1` | Reflection continuity + silhouette behavior |
| Overlay | `TOURMALINE_NORMAL_FIX_DEBUG=1` (± fix) | Where violations live, before vs after |
| Camera motion | (any of the above) | Temporal stability, no popping |

Example (deterministic capture, correction on):

```powershell
$env:TOURMALINE_NORMAL_FIX='1'; $env:TOURMALINE_AA='off'
$env:TOURMALINE_SSR='1'; $env:TOURMALINE_AUTO_EXPOSURE='0'
$env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
$env:TOURMALINE_CAPTURE='48'
& .\App.exe --pak content/paks/bistro.pak --disable_control --bench 64
```

Good probe content: a low-poly smooth-shaded sphere, a rounded metallic
handle, polished dielectrics, high-contrast environment. Beyond
screenshots, record GPU timings (`--bench`) and the max HDR luminance in
the edge regions (the artifact is an over-bright invalid sample).
