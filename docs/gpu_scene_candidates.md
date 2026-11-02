# GPU Scene v5: coarse CPU leaf selection + GPU instance verdict kernel

Stacked on **PR #34** (spatial leaves), which depends on **PR #31**
(mesh/LOD residency). GPUIR's split is preserved: CPU chooses coarse
spatial groups; GPU tests individual instances.

## CPU: PassedInstanceRanges

`SelectPassedInstanceRanges(db, LeafTickets, RelativeFrustum, mode)`
evaluates a *leaf* once, never looping over its member instances. Each
accepted `PassedInstanceRange` contains:

- `members.x`: first persistent LeafMember row
- `members.y`: count
- `members.z`: prefix offset in this view's candidate space
- `leaf.xy`: persistent Leaf slot + generation

The ordered ranges cover candidate space without gaps. The GPU
binary-searches `members.z` to identify each candidate's range.
No CPU per-instance material sorting or visibility test is required.

The CPU frustum is defined in **render-origin-relative coordinates**:
double-precision inward planes and a double world origin. Leaf centers
are reconstructed from DoubleFloat before subtracting the origin.
Each leaf AABB is tested against the six planes using projected extent.
Incomplete/non-finite bounds pass conservatively.

Invalid snapshots, duplicate leaf identities and capacity overflow
produce **no partial candidate set**. The caller retains legacy
rendering until candidate generation is fully valid. Caps are 65,536
leaf ranges and 1,048,576 instance candidates, separate from the
existing meshlet/draw caps.

## GPU: deterministic instance visibility verdict

`gpu_scene_instance_cull.cs.hlsl` is an opt-in compute shader, not
yet dispatched by WorldRenderer. Each thread owns one candidate and:

1. Binary-searches its prefix range.
2. Validates the generational leaf and member interval.
3. Reads the member and validates the referenced instance generation.
4. Validates the shared-mesh slot and generation.
5. Rebases DoubleFloat world translation against the view origin.
6. Tests the conservative instance sphere under an affine basis
   (Frobenius radius supports shear and nonuniform scaling).
7. Writes exactly one fixed-position verdict; incomplete bounds bypass
   rejection, and stale references generate a deterministic invalid
   verdict.

There is **no compaction**, GPU LOD transition, or draw generation yet.
A later pass will consume the verdicts and create work records.

The per-view GPU buffer contains a high/low origin and six inward
float32 planes. The compute push block contains BDA pointers and
row/range bounds. CPU and GPU must use a compatible relative frame.

## Safety and scope

- No Vulkan pipeline/dispatch is wired yet: the current float32
  absolute-world camera/View plane contract is not compatible with
  relative-space GPU instance transforms. First migrate the view
  matrices and origin consistently.
- CPU/GPU row-count and buffer capacity checks plus transfer barriers
  are required before enabling the kernel.
- Current GfxDraw, CPU pre-cull, HZB, seed/remainder and all actual
  rendering remain authoritative.
- The shader is committed as source; GPU readback parity testing and
  real-world benchmarks are required before enabling GPU visibility.

## Tests

`GpuSceneCandidatesTests` is headless. It covers near/far leaves,
incomplete bounds, signed cell coordinates, prefix offsets, duplicate
and invalid tickets, overflow failure, pass-all validation, same-cell
motion, generational slot reuse and 1e12-unit render origins.
