# Screen-space ray-traced sun shadows

The WorldRenderer's **Screen-space ray-traced sun shadows (experimental)**
checkbox extends the existing pre-Main sun contact-shadow compute pass. It
traces rays toward the sun through the **visible opaque depth buffer**, and
combines the resulting sun visibility with cascaded PCF / EVSM using
`min(shadowMapVisibility, tracedVisibility)`. It does **not** use Vulkan
ray-query, RTX, a triangle BVH, or acceleration structures.

## Enable

Open the viewer's **Lighting** controls and enable **Screen-space
ray-traced sun shadows (experimental)**. The pass replaces the short contact
march while this mode is enabled (the two modes share one visibility texture,
shader, render-graph synchronization, and material receive mask). The existing
contact shadows remain on by default; longer rays default to **off**.

The session-quality API (`RenderQualityConfig`) exposes
`screenSpaceRayShadowsEnabled`, distance, thickness, steps, refinement,
edge fade, and resolution divisor. The baseline contact controls are still
available independently. Supported distances are 0.5–30 m, steps 8–96,
refinement 0–8, and output resolution full or half; the default longer-ray
settings are 12 m, 64 steps, 4 refinements, 0.12 m thickness, and half-res.

The traced pass uses quadratic sample spacing (dense near the receiver),
binary depth-crossing refinement for coarse intersections, and screen-edge
visibility fading to avoid abrupt dark bands at the viewport border. Background
and out-of-view samples never occlude; the normal CSM/EVSM pass remains
responsible for out-of-view casters.

## Limits and validation

* **Screen-space limitation:** hidden or off-screen geometry cannot be
  intersected. This is not a replacement for shadow maps or hardware RT.
* Opaque depth only: transparent or masked geometry absent from the opaque
  prepass cannot cast traced shadows.
* Materials must have `MATERIAL_RECEIVE_CONTACT_SHADOWS` to receive
  the traced term, like the pre-existing contact pass.
* More steps/full resolution cost more GPU time; screen edges, thin geometry,
  disocclusions, and grazing rays can still show artifacts.
* Toggle the checkbox on and off while viewing the same sun-lit surface;
  compare both PCF and EVSM, full and half resolution, and the
  **Contact shadows** debug view. The existing
  `TOURMALINE_CONTACT_SHADOWS_DUMP=<frame>` also captures this pass's
  visibility texture.
* `RendergraphContractTests` pins the 64-byte push layout and the
  opt-in defaults. Visual and shader compilation validation require a
  Vulkan + DXC installation.
