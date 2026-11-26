# Screen-adaptive SDF surface irradiance cache

Status: draft architecture and CPU reference, not an enabled GPU renderer.
Complements the existing DDGI path; no change to ambient mode selection yet.

## Motivation

Use visible surfaces to request lighting rather than fill empty volume cells.
Persist diffuse irradiance on sparse SDF surface leaves/voxels so camera
motion does not discard expensive ray samples. A screen probe is a work
request, not necessarily a new persistent light probe.

## Placement and bounded work

After depth/geometric normals, reconstruct world positions with the exact
view inverse projection and depth convention. Reject background and invalid
samples. Choose a tile size from resolution and maxProbes so regular tiles
consume at most 75% of the budget. Classify samples using point-to-plane
distance, normal similarity and stable instance identity; select an actual
surface representative rather than averaging across a gap. Allocate a
second representative at discontinuities, prioritized by uncovered pixel
count and depth/normal separation. Keep a hard global cap even in complex
tiles; unresolved surfaces fall back or request next frame. Avoid splitting
a continuous sloped plane solely due to depth range.

Use perspective footprint = 2 * viewDepth * tanHalfFovY * tilePixels /
imageHeight to choose a power-of-two world cache level. Example spacing
0.25/0.5/1/2/4 m, with hysteresis at level boundaries. Neighboring level
transition uses compatible entries only. Orthographic views need their own
constant footprint. Use a small world-anchored coarse fallback for offscreen
or uncovered surfaces. The visible demand schedule is not a full radiance
cascade (no angular interval merging).

Expose maxProbes=512, updates=128, rays=32, maxStepsPerRay=24, traceDistance
=12 m, influenceRadius=6 m, cache capacity and memory cap. These are
starting values. 4096 rays and 98304 march steps per frame, before separately
budgeted hierarchy visits and normal taps; not a GPU-time estimate.
Prioritize invalid/new/old probes, with age-based starvation protection.
Cap voxel allocations, hash attempts, traversal node visits, candidate
tests, and reduction scratch. Distance cutoff alone cannot bound work.

## Geometry and identity

Reuse StaticSdfGpuScene and static_sdf_trace.hlsli. Existing leaves are
quantized constant samples in mesh-local octrees, not interpolated bricks.
This PR exposes the mesh-local leaf index captured before gradient taps,
and gradient-facing side, on each SDF hit. Absent child has an invalid
leaf index; never cache it. A hit instance index is a transient residency
index, not a persistent ID. GPU scene upload must publish a stable world
instance ID and geometry/transform revision separately before using cache.
The CPU reference keys by stable instance, geometry generation, local leaf,
brick voxel, normal side, and level. Distinct instances must not share
lighting even when their geometry asset is shared. Unsigned or degenerate
gradients need additional stable surface orientation validation; do not
assume normalSide alone establishes geometric identity.

Add world-space BVH or brick index over resident instance bounds. Traverse
front-to-back, skipping empty nodes only where occupancy/bounds are
conservative. In occupied leaves use conservative distance steps or leaf
exit traversal. Future brick leaves (e.g. 4^3 or 8^3) enable trilinear
distance samples and per-surface-voxel metadata. Do not confuse a truncated
distance with a conservative empty-space bound. Track true miss vs budget
exhausted vs missing coverage. The existing static SDF scene has 128
instances and skips masked/skinned/dynamic meshes; make residency independent
of SSR and account for trace origins outside the camera's immediate cell.
No hardware ray-query requirement.

## Lighting and accumulation

For each requested surface, offset along its geometric normal by a
voxel-aware bias. Trace cosine-weighted hemisphere rays (rotating sequence).
For a true miss sample sky without double-counting the direct sun disk.
For a hit, read compact base color, metallic and emissive. Evaluate
Lambertian outgoing radiance:

    Lout = emissive + (1-metallic)*albedo/pi *
           (E_sun + E_sky + E_previous_indirect)

Do not count sky twice: when estimating indirect irradiance from primary
rays, a hit's E_sky must represent incoming sky at that hit, not its outgoing
environment in the original ray direction. Previous cache irradiance must
have the same definition (sky-inclusive or indirect-only); use a single
consistent decomposition and avoid adding E_sky again if included in history.
Use analytic direct environment fallback for invalid history. Begin with
one bounce, then enable feedback with clamped diffuse reflectance and
energy-conservation tests. For cosine-weighted samples E = pi*mean(Li).
Store incoming irradiance per oriented surface voxel, not outgoing radiance;
albedo is applied once at the receiver. RGB is sufficient at a known normal;
use SH or directional bins only where normal variation warrants storage.

For multiple screen requests targeting the same key, reduce samples once
per voxel with sample weights, then EMA into persistent cache. Use bounded
hash probing with full-key verification, deterministic allocation and
eviction, invalid first samples bypass hysteresis. Read previous completed
history, write distinct next history, publish only after barriers; no
floating-point atomic requirement if sort/reduce is used. Cache survives
camera motion and residency repacks by stable keys; invalidate on world,
asset, transform or lighting epoch changes as appropriate. On gradual
lighting changes reduce hysteresis rather than reset everything.
Do not deposit a ray's incident radiance at the hit as if it were irradiance
of that surface: update a surface with rays sampled from that surface or
with a correctly weighted transport estimator.

## Offline visibility

Optional world-instance/voxel sidecar (versioned, separate from shared
mesh TSDF). Bake sky visibility and a 64-bit sun-path mask along the
configured trajectory including endpoints; interpolate visibility between
samples. For a fixed sun a single byte suffices. For changing azimuth/path
use a directional mask instead. Bake against the full static scene, not
the mesh in isolation. Directional masks approximate partial visibility;
dynamic occluders require runtime shadow correction. Missing or stale
bake uses bounded shadow trace or conservative fallback, never unconditionally
visible. A probe's own sun visibility is not the visibility at its ray hit.

## Optional tint-only GI and large-scale interior darkening

Keep FullIrradiance as the default proposed payload; TintOnly is a cheaper,
mutually exclusive diffuse ambient mode. InteriorDarkening and SunWeightedTint
are independent opt-in controls (the latter only affects TintOnly). The
existing renderer AO solutions remain in place. This is not full GI: tint
alone cannot add brightness, illuminate a black scene, or transport emissive
energy. Do not feed tint-only history back as physical bounce radiance.

### Compact chroma

Use signed Co/Cg from linear hit albedo rather than angular hue:
Co=(R-B)/2, Cg=G/2-(R+B)/4. For each cosine-weighted receiver ray that hits
a diffuse surface, accumulate the chroma weighted by distance falloff,
diffuse reflectance fraction and optionally the hit's current sun visibility.
A true miss contributes zero; divide by all scheduled valid samples, not
just hits. Reject incomplete estimates rather than treating traversal
exhaustion as a miss. Preserve absolute chroma so dark surfaces bleed less.
For sun weighting use lerp(skyWeight,1,hitSunVisibility), with skyWeight
chosen from current relative sky/sun contribution (night should not
suppress all tint). Optionally multiply by hit NdotL in the direct-sun
part. Missing bake uses unweighted tint or a separately budgeted shadow
query. Filter/accumulate decoded signed channels; quantize only at
publication. Opposing hues cancel, neutral materials yield zero.

Two signed 8-bit channels encode [-0.5,0.5]. A third UNORM8 channel
stores sky visibility; optionally pack as uint32 with spare byte for
confidence/flags (avoid requiring 8-bit storage features). Raw payload
is 3 bytes, practical aligned payload may be 4 bytes, excluding keys,
validity, history and allocation overhead. Full RGB FP16 uses 6 bytes.
No SH or directional radiance bins needed at an oriented surface.
Alternatively bake the tint offline for static geometry and materials:
no runtime bounce rays, but no accurate response to spatial lighting
changes. Dynamic tint can reuse the same adaptive probe budget and SDF
palette; skips expensive hit BRDF and secondary shadows when using bake.

At resolve apply chroma to the existing linear sky diffuse ambient
before receiver albedo, normalize to its original Rec.709 luminance,
then apply optional interior scale. Preserve direct light, local
lights, emissive and specular. A zero ambient input stays zero.

### Interior darkening is not another short-range AO

Use the same offline directional visibility infrastructure but distinguish:
- current sun visibility Vsun: directional direct light occlusion;
- sun-path coverage: weighted average across daylight directions, an
  optional low-confidence proxy for enclosure, not equivalent to sky view;
- Vsky: long-range cosine/directional-sky-weighted escape fraction
  normalized by the unoccluded response for the same orientation.
  A north-facing wall or an outdoor surface shaded by a tree can have
  Vsun=0 yet substantial Vsky. Night is not an interior.

Bake Vsky per world-instance oriented surface voxel using full static scene
geometry, or estimate it with separately bounded long-range rays/coarse
hierarchy. Use only confirmed sky escape; a ray ending at local 12 m
cutoff, missing resident geometry, or exhausting its budget is unknown.
For interiors a separate far cutoff (start 64–128 m, scene dependent),
coarse occupancy/portal connectivity or offline bake is important.
Short-range tint rays may also supply visibility when they conclusively
hit an occluder or reach sky, but do not assume all short misses escape.
A separate long-range visibility interval or offline visibility avoids
double-counting nearby AO: split near/far occlusion or use a calibrated
blend when the existing AO already contains the same obstruction.
Avoid receiver-only sun shadows modulating diffuse ambient directly.

Interior scale = lerp(1, lerp(interiorFloor,1,Vsky), interiorStrength).
Suggested initial floor 0.1 and strength 1 when enabled; art controls,
not physical constants. Apply only to the analytic sky ambient slice,
not baked/full GI that already represents enclosure, nor AO twice.
In FullIrradiance mode do not multiply already sky-occluded irradiance
by Vsky; use it only for the analytic fallback/residual sky slice.
Missing visibility uses valid previous/baked data or neutral scale.
Sun-path masks can guide a provisional classification, but never force a
fully dark result from absence of direct sun. Dynamic lights can still
light interiors even when Vsky is zero.

### Validation
- Tint disabled and interior disabled preserve existing lighting.
- Neutral/opposite colors, packing round trip, temporal interpolation,
  distance weighting and luminance preservation; no light in black input.
- Open outdoor shade vs sealed cave vs window/door/overhang, daytime and
  nighttime; avoid darkening local lights and emissive.
- Long-range visibility vs local AO overlap, missing bake/residency,
  per-instance side identity, temporal transitions and memory profiling.
- Compare FullIrradiance, TintOnly dynamic/baked, and visibility-only
  configurations. CPU reference only in this draft; runtime modes,
  visibility baking, shader integration and profiling remain TODO.

## Resolve and graph

Depth/normal prepass -> classify tiles -> compact/deduplicate requests ->
trace SDF -> reduce/cache update -> depth-aware resolve -> diffuse lighting.
If current render graph lacks appropriate normals before Main, first use a
dedicated prepass or previous-frame requests with disocclusion rejection;
verify graph scheduling rather than assuming resources exist.
Gather nearby probes only when point-to-plane distance, normal, instance
and visibility agree. For cross-surface/volumetric reuse use directional
distance moments or bounded visibility checks; a distance cutoff does not
stop light leaking through walls. Preserve baked/sky fallback, direct,
specular, AO and exclusive SSGI diffuse replacement ownership. Secondary
views do not advance the same history. Declare all BDA accesses in graph
hints and follow the DDGI cross-flight history lifetime contract.

## Comparison with DDGI

Existing DDGI traces a dense world-subcell probe grid via inline RayQuery
and stores directional irradiance and distance moments. Its current trace
shader is sky/occlusion diagnostic. This design is a surface cache with
screen-adaptive demand, sparse persistent voxel accumulation, coarse
material-aware SDF tracing, and optional baked visibility. It trades
offscreen receiver coverage for strict budgets. SDF tracing could also be
used for conventional DDGI; the tracing backend alone is not the distinction.

## Delivery and acceptance

- This draft: CPU reference for compatibility, footprint/level, budget,
  cache identity/EMA/eviction and sun-mask lookup; regression cases in
  test_sdf.cpp; SDF shader hit leaf metadata.
- Next: stable GPU identity, bounded traversal and residency independent
  of SSR; validate true miss vs incomplete trace.
- Next: adaptive GPU classification and scheduling, cache resources,
  ping-pong update and diffuse resolve behind an opt-in mode.
- Next: versioned offline visibility bake and optional feedback.
- Validate constant sky (E/pi = L), black scene, colored wall bounce,
  two rooms separated by thin wall, sloped tile vs depth edge, camera
  movement/teleport and cache repack, moving sun, shared mesh instances,
  missing coverage, no RT, multiple frames/views, toggling and SSGI.
- Build Debug/Release and shaders, SdfTests and text integrity suite,
  Vulkan validation and perf harness A/B (baseline, DDGI, SDF GI).
  Report actual GPU time, ray/step/node counts, cache hits, overflow,
  coverage, memory and temporal stability. No performance claim or GPU
  verification in this draft.

References: AMD Brixelizer GI, https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/
and DDGI, https://jcgt.org/published/0008/02/01/.
