# SG-GI Probe Volume — Math Writeup

Single-document description of the mathematics behind the current baked
global-illumination implementation: representation, bake-time transport,
fitting, placement, runtime interpolation, and final shading. Intended for
proofreading against the code.

Sources of truth:

- Representation / constants: `engine/include/Engine/GI/sg.h`,
  `engine/shaders/sggi_types.hlsli`
- Evaluation / diffuse / specular: `engine/src/GI/sg.cpp`,
  `engine/shaders/sggi.hlsli`
- Path tracer / sky / NEE: `engine/src/GI/bake.cpp`, `engine/include/Engine/GI/bake.h`
- Interpolation / grids / serialization: `engine/src/GI/probe_volume.cpp`,
  `engine/include/Engine/GI/probe_volume.h`
- Final shading: `engine/shaders/default_lit.frag.hlsl` (around line 753)
- Regression tests: `tests/test_gi.cpp`

CPU and HLSL are kept in exact parity by unit tests (`SgGiVolume::Sample`
mirrors `SgGiSample`; `SgCosKernel` mirrors `SgGiCosKernel`; basis tables
must match element-for-element).

---

## 0. Notation and conventions

- Directions are unit vectors on the sphere $S^2$. $d\omega$ is the solid-angle
  measure, $\int_{S^2} d\omega = 4\pi$.
- Radiance fields are RGB (three independent channels sharing one scalar
  basis). All bake math is linear HDR; colors are linear, never sRGB, except
  where the baker explicitly linearizes texture averages.
- $(x)_+ = \max(x, 0)$.
- $\pi = 3.14159265359$.

---

## 1. Radiance field representation: fixed-basis spherical Gaussians

Each probe stores an approximation of the full incoming radiance function
$L(\omega)$ as a sum of $N = 9$ spherical-Gaussian (SG) lobes with **globally
fixed** axes $\mu_i$ and sharpness $\lambda$, and **per-probe** RGB amplitudes
$a_i$:

$$L(\omega) \approx \sum_{i=0}^{8} a_i \, G_i(\omega), \qquad
G_i(\omega) = \exp\bigl(\lambda\,(\mu_i \cdot \omega - 1)\bigr).$$

Key properties:

- **Peak convention:** $G_i(\mu_i) = 1$, so $a_i$ is the lobe's peak radiance
  (a "radiance tap" in direction $\mu_i$). This is asserted by test
  (`sg lobe peak == 1`).
- **Fixed basis ⇒ safe interpolation.** Because $\mu_i$ and $\lambda$ are
  global constants (not per-probe data), blending two probes means blending
  amplitude vectors entry-wise; lobe correspondence is trivially preserved.
  This is the same representation choice as the dynamic-object probes in
  *Precomputed Global Illumination in The Order: 1886* (Hill/McAuley), cited
  in `sg.h`. A per-probeaxis basis would be more expressive but
  uninterpolable.
- **Sharpness $\lambda = 4.0$.** Each lobe has FWHM $\approx 68^\circ$:
  $G = 1/2$ when $\mu\cdot\omega = 1 + \ln(1/2)/4 \approx 0.827$, i.e. a
  half-angle of $\approx 34^\circ$. Nine such lobes capture broad directional
  structure (diffuse irradiance plus rough-glossy reflections). They cannot
  represent mirrors — smooth surfaces keep the analytic environment for
  specular (Section 9).

### 1.1 The nine axes

Golden-angle (Fibonacci) spiral, Y-up, unit length. Must match in C++ and
HLSL in the same order:

| lobe | $\mu_x$ | $\mu_y$ | $\mu_z$ |
| --- | --- | --- | --- |
| 0 | 0.45812285 | 0.88888889 | 0.00000000 |
| 1 | −0.54960231 | 0.66666667 | 0.50348074 |
| 2 | 0.07831653 | 0.44444444 | −0.89237641 |
| 3 | 0.59322548 | 0.22222222 | 0.77375759 |
| 4 | −0.98471349 | 0.00000000 | −0.17418195 |
| 5 | 0.82265807 | −0.22222222 | −0.52330773 |
| 6 | −0.23255520 | −0.44444444 | 0.86509376 |
| 7 | −0.34353981 | −0.66666667 | −0.66146500 |
| 8 | 0.43032455 | −0.88888889 | 0.15715383 |

Coverage: every direction sees a lobe within $\approx 57^\circ$
(unit test: worst best-dot over 5000 random directions $> 0.54$).

---

## 2. SG calculus used at runtime

### 2.1 Sphere integral of one SG

For $G(\omega) = a\,\exp(l\,(\hat\mu\cdot\omega - 1))$, align $\hat\mu$ with
$+z$ and integrate:

$$\int_{S^2} G(\omega)\,d\omega
= a \cdot 2\pi \int_{-1}^{1} e^{l(t-1)}\,dt
= a\,\frac{2\pi}{l}\,\bigl(1 - e^{-2l}\bigr).$$

The code factors this as `SgIntegral(a, l)`. As $l \to 0$ the SG flattens to
the constant $a\,e^{-l} \to a$ and the integral tends to $4\pi a$; the product
routine below handles that limit in closed form.

### 2.2 Product integral of two SGs (the workhorse)

The product of two SGs is another SG. With unit-amplitude basis lobe
$G_i$ (sharpness $\lambda = 4$, axis $\mu_i$) and an external
$SG(a_s, l_s, \mu_s)$:

$$G_i(\omega)\,SG(\omega)
= a_s\,e^{-\lambda - l_s}\,
  \exp\bigl((\lambda\mu_i + l_s\mu_s)\cdot\omega\bigr).$$

Let $v = \lambda\mu_i + l_s\mu_s$ and $l_+ = |v|$. Then the product is the SG
with sharpness $l_+$, axis $v/l_+$, and amplitude
$a_+ = a_s \exp(l_+ - \lambda - l_s)$:

$$\int_{S^2} G_i \cdot SG \,d\omega
= a_s\,e^{\,l_+ - \lambda - l_s}\,
  \frac{2\pi}{l_+}\,\bigl(1 - e^{-2l_+}\bigr).$$

This is exactly `SgKernelProduct` / `SgGiProductIntegral`. The degenerate
case $l_+ < 10^{-6}$ (near-opposite axes cancelling) falls back to the flat
limit:

$$\int \approx a_s \cdot 4\pi\,e^{-(\lambda + l_s)}.$$

---

## 3. Diffuse irradiance via a precomputed 1-D kernel

Diffuse irradiance for normal $n$ is

$$E(n) = \int_{S^2} L(\omega)\,(n\cdot\omega)_+\,d\omega.$$

Substituting the SG expansion, linearity gives

$$E(n) = \sum_{i=0}^{8} a_i \, K(\mu_i \cdot n), \qquad
K(t) = \int_{S^2} G(\omega; \mu, \lambda{=}4)\,(n\cdot\omega)_+\,d\omega,
\quad t = \mu\cdot n.$$

$K$ depends only on the scalar $t$ (rotational symmetry about $\mu$), so it
is a **fixed 1-D function**, numerically integrated once offline and fitted
with a degree-6 Chebyshev polynomial (worst 0.33% relative error):

$$K(t) \approx \sum_{k=0}^{6} c_k\,T_k(t), \qquad t \in [-1,1],$$

evaluated with the Clenshaw recurrence and clamped below at 0:

$$b_8 = b_7 = 0;\quad
b_k = 2t\,b_{k+1} - b_{k+2} + c_k\;(k = 6\ldots1);\quad
K = \max(c_0 + t\,b_1 - b_2,\, 0).$$

Coefficients (must match in C++ `kSgCosCheb` and HLSL `SG_GI_COS_CHEB`):

$$c = (0.4445942889,\; 0.5893779824,\; 0.1573046510,\; 0,\;
-0.0062341036,\; 0,\; 0.0002682013).$$

Notes:

- The kernel is smooth and strictly positive on $[-1,1]$ (fat lobe tails), so
  no angular cutoff is applied — only the numerical clamp at 0.
- Sanity: for a constant field $L \equiv c$,
  $E(n) = c\int (n\cdot\omega)_+ d\omega = \pi c$. The test suite fits a
  constant field and requires $|E - \pi c| / \pi c < 10\%$; a Monte-Carlo
  cross-check of general fields requires $< 12\%$.
- The caller divides by $\pi$ when forming the Lambert term
  (Section 9), matching the direct-lighting convention.

---

## 4. Rough specular via a normalized SG kernel

The ambient-specular path approximates the GGX reflection kernel
$K(w) = D(h(w))/4$ (view assumed along the normal — the standard
ambient-specular simplification) as a **normalized SG** centered on the
reflection vector $r$:

- $\alpha = \mathrm{rough}^2$ (shader GGX convention),
  $\mathrm{rough}$ clamped to $[0.04, 1]$.
- Sharpness $l_s = 1 / (4\alpha^2 + 10^{-4})$.
  Rationale in code: the GGX NDF is Gaussian-ish in $\tan\theta_h$ with
  $\sigma = \alpha$; reflection doubles the angle, so the $\omega$-space
  lobe has $\tan$-$\sigma$ $2\alpha$, i.e. SG sharpness $1/(4\alpha^2)$.
- Normalization $a_s = l_s / \bigl(2\pi\,(1 - e^{-2l_s})\bigr)$, which is
  exactly $1 / \int SG(1, l_s)\,d\omega$, so the kernel integrates to 1 and a
  constant environment reproduces itself (unit-tested: response to a fitted
  $0.7$ field stays within $0.08$ for roughness $0.3/0.5/0.8$).

Response:

$$S(r) = \sum_{i=0}^{8} a_i \int_{S^2} G_i(\omega)\,
SG(\omega; a_s, l_s, r)\,d\omega,$$

each term via the Section 2.2 product integral. At low roughness the kernel
sharpens into a mirror tap (unit test: aligned single-lobe field of amplitude
2 reads back $> 1.2$ at roughness $0.1$); a GGX importance-sampled
Monte-Carlo cross-check at roughness $0.55$ must agree within 20%.

---

## 5. Bake-time light transport (CPU path tracer)

### 5.1 What is baked, and what is deliberately excluded

| Source | Baked? | Why |
| --- | --- | --- |
| Analytic sky gradient | Yes, every vertex | Probes replace env-IBL ambient entirely |
| Sun bounce (depth $\ge 1$) | Yes (NEE) | Indirect sun is true GI |
| Emissive static geometry | Yes (direct at primary + NEE every vertex) | Runtime has no other emissive lighting path |
| Direct sun at primary vertex | **No** | Owned by runtime cascaded shadow maps; storing it would double-count |
| Dynamic point/spot lights | **No** | Runtime forward+ only, by design |

### 5.2 Scene and materials

World-space opaque triangle soup + BVH, extracted from a cook-mode cluster.
Per-triangle material: Lambertian albedo and HDR emissive:

$$\mathrm{albedo} = \mathrm{baseColor}\,(1 - \mathrm{metallic}), \qquad
\mathrm{metallic} = \mathrm{factor}\times \mathrm{MRtexture.B},$$

with base color tinted by the base-color texture's average texel and emissive
by the emissive texture's average (linearized). Glass (`Blend`) is skipped;
alpha-tested cutout is included by default.

### 5.3 Sky model

No sun disc (the sun is handled by NEE / shadow maps):

$$t = \mathrm{clamp}(d_y, 0, 1),\quad
\mathrm{sky} = \mathrm{mix}(\mathrm{horizon}, \mathrm{zenith}, t^{p})\,I_{sky},$$

and for $d_y < 0$:

$$\mathrm{sky} = \mathrm{mix}(\mathrm{horizon}, \mathrm{ground},
\mathrm{clamp}(-3d_y, 0, 1))\,I_{sky}.$$

CPU (`SkyRadiance`) and GPU (`EvaluateSkyGradient`) agree.

### 5.4 Path estimator (`TraceIncoming`)

State: radiance $L = 0$, throughput $T = 1$, primary flag. For depths
$0 \ldots$ `maxBounces` (default 3):

1. Miss → $L \mathrel{+}= T \cdot \mathrm{Sky}(d)$, stop.
2. Hit at $p$ with shading normal $n$ (geometric normal flipped to face the
   ray). Let $m$ be the surface material.
3. **Direct emission (primary vertex only):** if the hit triangle is
   emissive, $L \mathrel{+}= T \cdot L_e$. This is the only path for
   directly-visible emissive into the field; bounce-vertex on-hit emission
   is skipped because the previous vertex's NEE already accounts for direct
   lights.
4. **Emissive NEE (every vertex):** $L \mathrel{+}= T \cdot S_{\mathrm{emis}}$,
   the Lambertian reflected term (albedo included, see below).
5. **Sun NEE** (bounce vertices only):
   $$S_{\mathrm{sun}} =
   \begin{cases}
   0 & n\cdot l \le 0\ \text{or occluded},\\
   \mathrm{sunColor}\cdot I_{\mathrm{sun}}\,(n\cdot l)\,
   \mathrm{albedo}/\pi & \text{otherwise}.
   \end{cases}$$
6. $T \mathrel{*}= \mathrm{albedo}$; stop if $\mathrm{lum}(T) < 10^{-4}$.
7. Scatter $d \sim$ cosine hemisphere about $n$;
   $p \leftarrow p + n\cdot 10^{-3}\,\mathrm{m}$; primary $\leftarrow$ false.

### 5.5 Next-event estimators

**Emissive triangles** are sampled proportionally to $A\cdot\mathrm{lum}$:
select triangle $e$ with probability
$p_{\mathrm{sel}} = A_e\,\mathrm{lum}_e / \sum A\,\mathrm{lum}$, pick a uniform
point $y$ on it ($\sqrt{}$-mapping), set $w = y - p$, $d = |w|$,
$\hat u = w/d$. Reject if behind the surface ($\hat u\cdot n \le 0$),
grazing ($|\,n_e\!\cdot\!\hat u\,| < 10^{-4}$), or occluded
(`AnyHit` over $d - 2\varepsilon$). The joint area pdf is
$p_{\mathrm{sel}}/A_e$, so the total solid-angle pdf is:

$$p_{SA} = \frac{p_{\mathrm{sel}}\,d^2}{A_e\,|\,n_e\!\cdot\!\hat u\,|}.$$

Lambertian contribution (current-vertex albedo included):

$$S_{\mathrm{emis}} = L_e \cdot \mathrm{albedo}\,
\frac{(\hat u\cdot n)/\pi}{p_{SA}}.$$

**Irradiance reference** for refinement: cosine-hemisphere sample
$L$ and scale:

$$E(n) = \pi \cdot \overline{L},$$

which is the $E = \pi\cdot\mathrm{mean}(L)$ estimator (cosine-weighted pdf
cancels the cosine term).

---

## 6. Fitting the basis (non-negative least squares)

Radiance samples $(d_s, L_s)$, $s = 1\ldots M$ (default $M = 128$), uniform
on the sphere, each with solid-angle weight $w = 4\pi/M$. Minimize per
channel:

$$\min_{a \ge 0}\ \sum_{s} w\,\Bigl\lVert
L_s - \sum_{i} a_i\,b_i(d_s)\Bigr\rVert^2,
\qquad b_i(d) = G_i(d).$$

Online accumulation (`SgFitAccumulator`): scalar Gram matrix (shared by all
channels) plus RGB right-hand sides,

$$\mathrm{Gram}_{ik} = \sum_s w\,b_i b_k, \qquad
\mathrm{rhs}_i = \sum_s w\,L_s\,b_i.$$

Solve:

1. **Damped normal equations.** The overlapping lobes make Gram
   near-singular for sparse/biased samples, so a damping ladder is tried:
   $C = \mathrm{Gram} + \delta I$ with
   $\delta = \max(\mathrm{tr},10^{-6})\cdot 10^{\mathrm{level}-6}/9$,
   $\mathrm{level} = 0\ldots4$; the first level whose Cholesky factorization
   succeeds wins (9×9, in place).
2. **Non-negativity.** Clamp $a \leftarrow \max(a, 0)$, then refine with 32
   damped multiplicative (NMF-style) updates that recover energy lost to
   clamping while staying non-negative:
   $$a_i \leftarrow a_i \left(
   \max\left(\mathrm{rhs}_i / \max((Ca)_i,\,10^{-12}),\,10^{-4}\right)
   \right)^{1/2},$$
   i.e. the ratio is floored at $10^{-4}$ to avoid oscillation on noisy fits.
3. Clamp to $[0,\,65504]$ (fp16 ceiling).

Tests: fields exactly in the basis span are recovered to $< 2\%$; fitting a
negated field still yields all-non-negative amplitudes.

---

## 7. Adaptive placement (bricks → probes)

### 7.1 Brick lattice and geometry-driven refinement

The scene bounds (padded by `rootBrickSize/4`) are covered by a coarse grid
of root bricks (`rootBrickSize = 8 m`). Each brick spawns **one** probe at
its (relocated) center, so brick size sets probe spacing. Refinement is an
octree split: one brick → 8 children at center $\pm$ size$/4$ with size$/2$.

Geometry stage (breadth-first, one octree level at a time so no region
starves another under the probe budget):

- For level bricks of size $s$, with half-diagonal $h = s\sqrt{3}/2$:
  $$\mathrm{near} \iff
  \bigl(\mathrm{distToGeom}(\mathrm{center}) - h\bigr) < s\cdot f_{\mathrm{near}},
  \qquad f_{\mathrm{near}} = 1.0.$$
- Subdivide **all** candidates at the level only if
  $\mathrm{leaves} + 7\cdot\mathrm{candidates} \le
  \max(64,\,0.6\cdot\mathrm{maxProbes})$; otherwise every region stays at
  this depth and only error waves may go deeper.
- Stop at `minBrickSize` ($0.25$ m) or 8 levels.

### 7.2 Probe relocation

Probes must live in free space. Required clearance:
$\max(0.03\,\mathrm{m},\,0.2\cdot s)$:

1. Keep the center if it clears.
2. Else march away from the closest surface point in steps of $s/4$.
3. Else sweep 14 directions (6 axes + 8 cube diagonals).
4. Fully embedded bricks are **dropped** (never soft-accepted): a probe
   hugging geometry bakes dark occluded radiance and poisons its
   neighborhood with dark dots; leaving the region to neighbors is better.

### 7.3 Per-probe bake

At the relocated position: trace `raysPerProbe` (128) uniform-sphere samples
with `TraceIncoming`, fit Section 6, then record **6-axis visibility**:
raycast $\pm x, \pm y, \pm z$, obstacle distance capped at
`visibilityMaxDistance` ($2.5$ m).

### 7.4 Error-driven refinement (lighting complexity, not geometry)

Each baked probe is validated at 2 off-center test points
($+0.29/+0.11/−0.23$ and $−0.17/−0.21/+0.31$ times brick size) against the 3
axis normals. Reference: `TraceIrradiance` with `errorRaysPerProbe` (32)
rays. Reconstruction: the probe's own `SgDiffuseIrradiance`. Relative error:

$$\mathrm{err} = \frac{1}{6}\sum_{j=1}^{6}
\frac{\mathrm{lum}\,|\,E^{\mathrm{ref}}_j - E^{\mathrm{rec}}_j\,|}
{\max(\mathrm{lum}(E^{\mathrm{ref}}_j),\,0.02\cdot\overline{\mathrm{lum}})}.$$

Subdivide the brick if $\mathrm{err} > 0.12$ (`errorTolerance`), the level
and size allow it, and the probe budget (`maxProbes` $= 262144$) holds, for
up to `maxRefineWaves` ($8$) waves. This is the pass that spends probes on
doorways, corners, and sharp indirect gradients while a blank uniformly-lit
wall spends none.

### 7.5 Support radius and lookup grids

- Support radius: $R = 1.25 \times \mathrm{brickSize}$ (`probeSupportFactor`,
  meters). Overlap $> 1$ keeps blends smooth under visibility rejects.
- Per-level uniform grid with cell size $2\times\mathrm{brickSize}$ (meters),
  so a probe sphere (diameter $2.5\times\mathrm{brickSize}$) overlaps up to
  3 cells per axis (27 cells total) and each cell's probe list is a superset
  of everything that can shade a point inside it.
- Level extent = probe positions padded by radii; per-cell caps
  (64 probes/cell) and global caps keep pathological inputs bounded.

### 7.6 Diffusion (anti-polka-dot passes)

1. **Placement diffusion:** deterministic jitter ($\pm 0.2\,s$, clamped to
   the brick) breaks lattice symmetry; then up to 8 repulsion-relaxation
   passes spread same-level probes toward $0.85\,s$ spacing (steps of
   $0.2\,s$), always respecting the free-space guard.
2. **Visibility re-trace** from final positions.
3. **Radiance diffusion:** 3 Laplacian (heat-equation) passes over the probe
   neighbor graph with step $\lambda = 0.6$ (Jacobi):
   $$a \leftarrow a + 0.6\,(\bar a - a),$$
   where $\bar a$ is the Wendland-weighted neighbor mean — same level at
   full weight plus adjacent levels at half weight (this kills parent/child
   seam disagreement where error refinement created mixed-density regions).

---

## 8. Runtime interpolation

For shading point $p$, gather from **every level's** containing cell
(coordinates clamped into the grid) and blend amplitudes.

### 8.1 Spatial kernel (partition of unity)

$$w(p, q) =
\begin{cases}
0 & |p - q_{\mathrm{pos}}|^2 \ge R^2,\\
\bigl(1 - |p - q_{\mathrm{pos}}|^2/R^2\bigr)^3 & \text{otherwise}.
\end{cases}$$

A $C^2$-smooth compact kernel (Wendland-style $(1-x^2)^3$ in the squared
radius), zero at the support radius. Finer nearby probes naturally dominate
because their kernels are narrower and taller per unit volume.

### 8.2 Baked wall awareness (leak fix, no runtime rays)

Each probe stores 6 obstacle distances $v_k$. With
$d = p - q_{\mathrm{pos}}$ and
$\mathrm{slack} = \mathrm{clamp}(0.35\,R,\,0.05,\,0.30)$:

$$f_k =
\begin{cases}
1 & d\!\cdot\!e_k \le v_k + \mathrm{slack},\\
\max\!\left(0,\,1 -
\dfrac{d\!\cdot\!e_k - v_k - \mathrm{slack}}
{0.25\,v_k + 0.02}\right) & \text{otherwise},
\end{cases}
\qquad F = \prod_{k=1}^{6} f_k,$$

skipping axes with $v_k \le 0$ (no obstacle recorded). The slack keeps
on-surface points (which live *at* the recorded distance, plus fp16/normal
noise) alive, while the $25\%$-of-distance ramp prevents thin obstacles from
nuking open-air coverage. Combined weight: $w_v = w\cdot F$.

### 8.3 Blend and fallback

Primary (anti-leak) blend — visibility-weighted partition of unity:

$$a_i = \frac{\sum w_v\,a_i^{(q)}}{\sum w_v},
\qquad \text{requires } \sum w_v \ge 10^{-4}.$$

Fallback (deep clutter where every covering probe is visibility-rejected):
inverse-square blend of the 4 nearest **covering** probes
($w_n = 1/\max(|d|^2, 10^{-4})$):

$$a_i = \frac{\sum_{k<4} w_n^{(k)} a_i^{(k)}}{\sum_{k<4} w_n^{(k)}}.$$

Blending *all* occluded probes would leak far-side dark radiance wholesale;
the nearest few are almost always same-side ("own brick") probes, and the
weighted blend stays smooth instead of collapsing to one-probe dots. If
neither path covers the point, the sample reports uncovered (magenta in the
debug view; shader falls back to the analytic ambient for that pixel).

---

## 9. Final shading combine

Per pixel with normal $N$, view $V$, reflection $R = \mathrm{reflect}(-V, N)$:

- $E_{GI} = \mathrm{SgGiDiffuseIrradiance}(a, N)\cdot I_{GI}$,
  $$\mathrm{diffuse}_{GI} = k_D \cdot \mathrm{baseColor}\,
  \frac{E_{GI}}{\pi}\cdot \mathrm{occ}\cdot \mathrm{AO},$$
  with $k_D = (1 - F)(1 - \mathrm{metallic})$ and
  $F = \mathrm{FresnelSchlickRoughness}(N\!\cdot\!V, F_0, \mathrm{rough})$.
- $S_{GI} = \mathrm{SgGiSpecular}(a, R, \mathrm{rough})\cdot I_{GI}$,
  $$\mathrm{spec}_{GI} = S_{GI}\cdot F_{\mathrm{env}}\cdot
  \mathrm{occ}\cdot \mathrm{AO},$$
- Roughness cross-fade (9 broad lobes cannot do mirrors):
  $$t = \mathrm{sat}\!\left(\frac{\mathrm{rough} - 0.15}{0.25}\right),\qquad
  \mathrm{spec} = \mathrm{mix}(\mathrm{spec}_{\mathrm{analytic}},
  \mathrm{spec}_{GI},\,t).$$

When covered, the probe field **replaces** the analytic sky ambient (it
already contains the occluded sky). Debug views: (1) flat
$E_{GI}\cdot I_{GI}\cdot\mathrm{exposure}$ without albedo; (2) diagnostics
$(1,\ \mathrm{sat}(W/4),\ \mathrm{sat}(\mathrm{count}/16))$,
violet-tinted on fallback, magenta when uncovered.

---

## 10. Storage, packing, serialization

- **GPU probe: 96 B** (`SgProbeGpu`): `float4` position + support radius;
  27 half-float RGB amplitudes in lobe-major order
  ($h = \mathrm{lobe}\cdot 3 + \mathrm{channel}$, word $h \gg 1$, half
  $(h\,\&\,1)\cdot 16$; 5 halves spare); 6 half-float visibility distances;
  flags (bit 0 = valid, bits 8–15 = level). Units: positions, support radii, and visibility distances are meters (float32 CPU/disk; visibility fp16 on GPU/disk); amplitudes are linear-HDR radiance (float32 CPU, fp16 GPU/disk, clamped to [0, 65504]).
- **Disk record: 88 B** + LZ4-compressed `.sggi` (header: magic `SGGI`,
  version 1, probe/level/lobe counts, sharpness 4.0, bounds, per-level cell
  sizes). Header mismatch (lobe count / sharpness) forces a re-bake.
- **Grid header: 544 B** (`SgGiGridGpu`): global bounds + probe count,
  level count, per-level cell size / origin / dims / cell / index / probe
  offsets. Cells are `uint2` (start, count); indices are `u32` probe ids.

---

## 11. Defaults and validation anchors

| Parameter | Default | Role |
| --- | --- | --- |
| `raysPerProbe` | 128 | Uniform-sphere radiance samples per probe |
| `maxBounces` | 3 | Diffuse vertices after the first hit |
| `errorRaysPerProbe` | 32 | Reference irradiance rays per test point |
| `rootBrickSize` | 8 m | Coarsest spacing |
| `minBrickSize` | 0.25 m | Finest spacing |
| `nearSurfaceFactor` | 1.0 | Geometry-subdivision reach |
| `errorTolerance` / `errorFloor` | 0.12 / 0.02 | Lighting-refinement threshold / luminance floor |
| `maxProbes` | 262144 | Hard probe budget |
| `maxRefineWaves` | 8 | Error-refinement iterations |
| `probeClearanceFactor` / `probeMinClearance` | 0.2 / 0.03 m | Free-space requirement |
| `visibilityMaxDistance` | 2.5 m | 6-axis distance cap |
| `probeSupportFactor` | 1.25 | $R = 1.25\times$ brick |
| `diffusionIterations` / `smoothingPasses` / `smoothingLambda` | 8 / 3 / 0.6 | Placement + radiance diffusion |

Validation (`tests/test_gi.cpp`, no GPU required): basis coverage/peak,
fit round-trip + non-negativity, diffuse vs Monte Carlo ($<12\%$) and
constant-field $\pi c$ ($<10\%$), specular normalization + mirror tap + GGX
Monte Carlo ($<20\%$), BVH hand-checked hits/distances, `.sggi` round-trip
with per-level counts and sampling coverage on both levels, fp16 pack
round-trips for amplitudes and visibility.

## 12. Known limitations (by design)

- Direct sun and dynamic lights are never in the field (Sections 5.1, 9).
- 9 broad lobes: diffuse plus rough-glossy only; sharp reflections stay
  analytic.
- Lambertian-only bounces; metallic surfaces bounce
  $\mathrm{baseColor}\,(1-\mathrm{metallic})$.
- Fully static bake: moving geometry or sun/sky changes require re-baking.
