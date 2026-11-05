# TODO — merged-as-draft follow-up work

PRs #136–#142 and #145/#148/#151/#152/#153/#157/#159 were merged
while still drafts (owner request, 2026-11-04).
Each entry records the work the PR author explicitly flagged as incomplete
in its own description. Local gates (full all-targets build + ctest +
Python tooling tests) verified compilation and existing regressions; the
items below are the remaining author-declared gaps.

## #136 — HDR screen distortion / heat shimmer
- GPU validation not run in the authoring environment: Vulkan validation
  layers, resize/reformat lifecycle, and identity/neutral-map behavior
  need a GPU-capable pass.
- Output-quality and performance validation (pre-bloom/tonemap insertion
  point, optional flow-map binding) still to be reviewed on real scenes.

## #137 — Ground dust cloud particles
- Soft depth *fading* is not implemented (depth testing exists; the
  fragment shader needs a graph-safe sampled-depth path).
- Ground height assumes a locally flat plane at the emitter position;
  terrain-following heightfield sampling is a follow-up.
- Overlapping alpha sprites are not depth-sorted; consider sorting or
  weighted blended OIT for dense clouds.
- Native build, GPU validation and capture tests were never run by the
  author (static source review only) — the local merge gate compiled and
  ran the CPU suite; visual validation remains open.

## #139 — Day/night solar preview
- Save/reopen persistence and scrubbing interaction not yet validated.
- Shadow behavior when the sun is below the horizon not yet validated.
- Editor preview only: no cooked-runtime serialized cycle or gameplay
  clock — the app runtime will not autonomously advance time; a scene
  directional light override path needs defining.
- (Merge note: the standalone smoke test was unregistered and
  assert-only; it is now `EditorDayNightTests` with `#undef NDEBUG`.)

## #140 — Opaque thin-surface backlighting
- HLSL variant compilation verified by the merge gate, but render
  validation of sun/point/spot rear contributions on leaves, cloth and
  paper is still open.
- Rear-light contribution is unshadowed by design — verify strength
  stays modest on real assets.
- Author's C++/test-run checklist items beyond the merge gate (render
  tests) remain open.

## #141 — Particle source graph
- Stacked on #137; merged last after retarget. Author-declared validation
  gaps: see the PR body's checklist (typed graph topology coverage and
  editor brush workflow validation were not completed).

## #142 — Scoped 15-frame GPU snapshot capture
- **Runtime validation DONE (2026-11-04, post `438cd288`)**: the full
  capture matrix records + verifies clean with pixel checks —
  {scoped, legacy} × {png, raw} × {1080p, 4K render-scale 2} ×
  {15, 16, 30} frames, including the 4K raw 30-frame worst case (the
  >256 MiB staging-split path). Same-mode reruns are near-identical
  (avg channel delta 0.23) — determinism holds.
- (Superseded by #148 for the aggregated-readback item.)

## #148 — Scoped snapshot single-batch readback + raw output
- (Resolves the #142 aggregated-readback TODO item above.)
- **Runtime validation DONE (2026-11-04, post `438cd288`)**: all matrix
  legs above record + verify clean; the 4K staging split works.
- **RESOLVED — capture-mode divergence was measurement noise**: on the
  fully-pinned `sponza-atrium` scenario camera (AA off, auto-exposure
  off, fixed cam), scoped vs legacy are pixel-identical across 30
  frames (avg/max delta 0.000) and scoped reruns are bit-identical.
  The earlier ~5.45 avg-delta reading came from unpinned ad-hoc runs
  (AA jitter phase + auto-exposure convergence drift between separate
  App launches), not the capture path. Both modes are interchangeable
  baselines under pinned environments.
- (Merge note: the branch never compiled — the RHI call was declared
  only on the private VulkanResourceManager; the public
  `ResourceManager::RecordSnapshotImageCopy` facade + forwarding impl
  were added in the merge.)
- **The entire matrix was blocked until `438cd288`**: master's App
  crashed on every workload (see the #151 entry — the DS-viz
  shader-define regression) — the crash was found BY this validation
  run, GPU-bisected over 53 revisions, and fixed.

## #145 — TLAS/BLAS debug inspector
- No local C++ compile, CI or Vulkan GPU run by the author; the merge
  gate compiled it and the CPU `SdfTests` assertions pass, but a real
  GPU session (RT sun shadows on, inspector panel open, collapsed-view
  no-scan claim verified) is still open.
- Counts are CPU build CANDIDATES only — Vulkan may further reject
  geometry; GPU AS memory/compaction/build timings are not reported.

## #148 — Scoped snapshot single-batch readback + raw output
- (Resolves the #142 aggregated-readback TODO item above.)
- GPU validation still open: real 15/16/30-frame recordings at 1080p
  and 4K in both `scoped` and `legacy` capture modes, PNG and raw
  formats, including the 4K >256 MiB staging split path.
- The 4K batch split during export (staging-limit respect) is untested
  against real output.
- (Merge note: the branch never compiled — the RHI call was declared
  only on the private VulkanResourceManager; the public
  `ResourceManager::RecordSnapshotImageCopy` facade + forwarding impl
  were added in the merge.)

## #151 — Zero-off-state tile diagnostics
- Author never built or GPU-tested; the merge gate compiled both the
  production and `.viz.spv` instrumented variants. Still open: Debug
  and Release Editor runs toggling Off → count heat → light-scoped →
  Off, shader hot-reload of both variant sets, and verifying the
  shipping profile still excludes diagnostics.
- **REVERTED on master (`438cd288`)**: the commit `b640cf15` (merged
  via this PR) broadened `DS_VIZ_ON` to every top-level non-shipping
  tree, which force-compiled `DS_DEBUG_VIZ` into EVERY shader of plain
  Release App builds while App's C++ never allocates the viz telemetry
  buffers — the shaders' out-of-bounds telemetry stores corrupted host
  memory (heap corruption / access violations at load or teardown,
  100% reproducible; GPU-bisected over 53 revisions and isolated with
  5/5-crash vs 5/5-clean repeat runs). The default is back to
  Debug-auto + explicit opt-in. The editor-Release-diagnostics goal
  needs TARGET-SCOPED instrumentation (viz shader variants + Editor-only
  C++ define; never a tree-wide shader define) before it returns.

## #152 — Terrain NaN-clipped holes + RGBA vertex attributes
- Author never built or ran tests; the merge gate compiled the shaders
  and runs `TerrainBlocksTests` (one far-grid expectation fixed in the
  merge: the 16m far step keeps vertex 1 inside the 32m hole texel).
- NaN clip-space vertex handling is DRIVER-DEPENDENT — the fragment
  alpha `clip` fallback exists, but a near/far LOD visual pass on the
  real GPU (NVIDIA + the Linux/AMD matrix if applicable) is open.
- Holes affect visual terrain only; heightfield collision and
  raymarch shadows unchanged — verify a holed tile still collides.

## #153 — Dockable top-down terrain World Map pane
- Author never compiled or GPU-tested; the merge gate built it clean.
  Still open per the author's checklist: multi-tile terrain creation,
  painting across seams, undo/redo, save/reload, pan/zoom/fit, dock
  layout reset integration, and View > Panels wiring.
- First slice edits procedural TerrainGroupData only — the streamed
  world H/S atlas, material painting and streamed world tiles are
  declared follow-ups.

## #157 — Modularize editor UI panels/widgets/actions
- Author never compiled (no local toolchain); the merge gate built it
  and its structural test runs, plus the conflict resolution note:
  the branch's "Render Settings" PanelRegistry entry was dropped to
  preserve #155's Options-window consolidation, and the #153 World
  Map panel keeps a standalone Begin/End block until registry
  migration.
- Author's manual runtime regression checklist remains open: saved
  dock layouts + View menu persistence, viewport hover/capture,
  outliner/inspector selection sync, property undo/redo + drag
  coalescing, asset navigation/import/DCC/thumbnails, modals and
  keyboard focus.

## #159 — Docked live scene composite offsets
- Author never ran a native Vulkan build or visual smoke test; the
  merge gate compiled it and the source-level regression check passes.
  Still open: visual verification of scene content in a docked
  Viewport at desktop origin AND after moving the editor window,
  including splitter resize and inactive-tab behavior.

## 2026-11-04 (evening) — audit-fix + wizard + asset-repo barrage

PRs #177–#182, #186, #188–#192 were merged on owner request the same
evening (several still drafts, all titled "implementation planned" in
the audit series). Local gates (full all-targets build + ctest + the
Python tooling tests) verified compilation and regressions on every
merge; the items below are the author-declared gaps from each audit
doc's STATUS line, plus the defects the blind merges introduced that
were fixed post-merge.

**Run-here validation battery (2026-11-05)** — every automated check
that could run on this dev box has run; the corresponding bullets are
removed from the entries below. Done and green: #199 (build + DXC +
107/107 ctest with its own particle regressions), #198 (the Vulkan
validation-layer session with the custom AA mode — clean, no VUID
errors, capture produced), #201 (an end-to-end single-machine
scenario_matrix run — `pass` record), #192 (the
TOURMALINE_SHADER_SWAP=20 frame-safety pin — 0.00% differing pixels),
#191 (bit-exact pre/post decomposition parity), #198 parity gate
(custom resolve vs hardware average: mean 0.001, 0.37% differing).

## Remaining work — classified

**EASY MECHANICAL TARGETS (no big design; safe to pick up next):**
1. ~~scenario_matrix extensionless-exe tolerance~~ **DONE (`7c675b01`)**:
   resolve_exe + unit tests; the example matrix spelling works on
   win32.
2. ~~Declare the hot-reload pins in RunConfig~~ **DONE (`7c675b01`)**:
   TOURMALINE_SHADER_RELOAD/_SRC/_SWAP join kKnown — the "unknown var
   (typo?)" warning is gone (verified live: clean swap-arm messages
   only).
3. Proofread backlog items 1-5 (the legacy block below): pak.cpp
   overflow guards, impl.cpp barrier assert+log, bindless fail-closed,
   gfxLodFilterable null-guard, NewFrame transient reuse — each is a
   small, test-covered change.
4. [Audit 01] #182's remaining CALLER MIGRATION — ~80 direct
   localTransform/globalTransform write sites (engine/editor/tests),
   each needing the loader-vs-live-writer classification from the
   scene.h contract; mechanical but substantial. The randomized
   snapshot-parity tests are the bigger half, see below.

**DESIGN / MAJOR WORK (needs an owner plan, not a quick pass):**
- [Audit 01] randomized CPU/GPU snapshot-parity test harness (#182).
- [Audit 09] crash recovery for interrupted publications (#190 — the
  receipt/staging side landed; recovery semantics are unwritten).
- [Audit 10] orchestration extraction stages beyond the landed
  config/descriptor collaborators (#191).
- [Audit 08] the real TLS gateway deployment/validation (#189 —
  environment work, not code).
- #201's golden-sequence comparisons + cross-machine aggregation
  (blocked on a baseline policy decision).
- #186's automated release pipeline (replaces the manual dispatch +
  tag steps).
- #199's material-aware PBR particle draw + prototype registration UI.

**MANUAL / HARDWARE-DEPENDENT (cannot run headless here):**
- #187 editor-session checklist (save/revert/close/failed-open, resize).
- #193 F1/F2 repro + the detached-lane manual smoke (moved window,
  splitter drags, non-100% DPI).
- #194 VRR behavior on a VRR-capable monitor (pair with --vsync on).
- #177 pen-input on real tablet hardware.
- #199's GPU runtime smoke + perf bench (needs authored mesh-emitter
  scene content, which no release pak currently carries).
- #198's silhouette/coverage eyeball (the A/B captures exist under
  out/msaa-ab* — a human review away from closing).


## #177 — SDL3 tablet pen input
- Merged as a draft with no completed CI. Post-merge master runs
  (CI + Tests + Shared asset repository on `3343a0f3`) were all green.
- No author-declared validation checklist was published; pen-input
  behavior on real hardware remains unverified.

## #178 — classic Transpose action line
- Merged as a draft with no completed CI; covered by the green
  post-merge runs above.

## #180 — shared asset installation hardening
- Merged with CI pending; the Shared asset repository workflow went
  green on the follow-up head.

## #181 — server-side user registry + permission groups
- Author note: "Tests were not executed locally because this
  environment cannot resolve GitHub; please confirm CI before merging."
- (Merge note: `test_asset_permissions` failed on windows-latest only —
  GitHub runners expose TEMP via the 8.3 short name `RUNNER~1` while
  configure_host normalizes to the long form; the reinstall-preservation
  assertion now compares resolved paths, `15ab8f3a`.)

## #182 — [Audit 01] scene mutation journaling (partial)
- STATUS: generation-checked transform setter, animation migration and
  dynamic-root journal fix committed. **Complete caller migration and
  integration tests remain.**
- Acceptance criteria not yet met: randomized + deterministic CPU/GPU
  snapshot parity tests for reparent/remove-reuse/transform/visibility/
  binding mutations.
- (Merge note: the new SetLocalTransform joined scene.h's GLOBAL compat
  bridge, growing R9's baseline 66→67; the re-export was retired and the
  11 call sites now spell `tour::scene::SetLocalTransform`, `f43c5059`.)

## #186 — project setup wizard + installer
- (Merge note: the generated `tools/setup_wizard.py` carried literal
  `\n` sequences instead of line breaks — a SyntaxError on import; fixed
  in `d18d93ba`. The shared-asset workflow union also had to be resolved
  by hand against #181's additions.)
- The GitHub Release / Windows-installer automation documented on the
  branch is MANUAL dispatch + post-merge git tag steps until the release
  workflow merges; no automated release pipeline exists yet.

## #188 — [Audit 07] stable prefab node identity (partial)
- STATUS: UUID-first preview matching and authoring, fail-closed
  duplicates, legacy fallback committed. **Identity migration and editor
  verification remain.**

## #189 — [Audit 08] asset repository transport (partial)
- STATUS: integrated onto per-user ACL-enabled master. **Real TLS
  gateway validation still required.**
- (Merge note: this PR's `_ALLOWED_ACTIONS` guard is what makes
  `source.write`/`product.write` fail closed — #181's test expected it
  but #181's own implementation lacked it; the failing assertion only
  passed once this PR landed.)

## #190 — [Audit 09] transactional publication (partial)
- STATUS: unique staging, publish lock, corrupted-dedup refusal,
  multi-manifest safe cleanup and regression tests committed.
  **Provenance receipt and crash recovery remain.**
- (Merge note: `executor.pak-args` still asserted the pre-isolation
  `.staging` path; publication now stages under
  `.staging/build-<nonce>/` so the test pins the contract instead,
  `f43c5059`.)

## #192 — [Audit 05] single-source shader variants
- **Validation DONE (2026-11-05)**: the manifest test suite passes, CI
  green from `3343a0f3` onward, and the TOURMALINE_SHADER_SWAP=20
  frame-safety bench pin reproduces the documented contract with the
  new manifest — 0.00% differing pixels vs the no-swap reference.
  (Out-of-band note: RunConfig warns "unknown TOURMALINE_* var" for
  TOURMALINE_SHADER_SWAP because the reload service reads it directly —
  harmless; declaring it in RunConfig.h is on the easy-target list.)

## #193 — docking overlays + camera mouse ownership
- **Detached scene-viewport LANDED (`4cf3b716`)**: the Viewport panel can
  undock into a secondary OS window — the live view's inline LDR target
  is bound as an ImGui user texture and presented by ImGui's backend in
  the detached window's swapchain (the offscreen scene render target
  the PR's scope note deferred). Diagnostic view modes (the legacy
  editor passes) remain main-window-only with an in-panel hint; camera
  pick/orbit math still assumes the primary window's input space; the
  inline extent still clamps to the main swap extent.
- Still open: the reported F1/F2 disappearance is neither reproduced nor
  fixed (the PR body's own disclaimer), and the author's manual smoke
  list (moved window, floating/re-docked splitter drags, non-100% DPI)
  has not been run against the detached lane.

## #194 — VRR-aware frame pacing + VSync controls (merged as draft)
- (Merge note: `app/src/Host.h` carried the same literal-backslash-n
  generation artifact as the setup wizard — the `vrr` member was
  swallowed by the previous line's comment and PakAppArgs/PakApp failed
  C2039; fixed in `2a68dd34`.)
- Hardware validation NOT run: VRR cap behavior (refresh-detect, the
  2-FPS margin, mode-change updates) needs a real VRR-capable monitor
  with matching OS/driver settings, paired with `--vsync on` — see
  docs/vrr_frame_pacing.md. CPU-only cap-selection tests pass.
- Merged as a draft with CI pending; post-fix master runs must confirm
  both platforms (the breakage above would have failed them).

## #183 — [Audit 02] GPU scene record readiness
- Author note: "Tests were added but not executed here" (integrator-owned
  validation). Done post-merge: full build + 107/107 ctest green
  (`a06a7bec` era).

## #185 — [Audit 04] WorldPartition provider contracts
- Same author note; same integrator validation (green post-merge runs).

## #187 — [Audit 06] per-world editor session state (recovery/selection)
- Still open (author checklist): validate save/revert/close/failed-open
  paths and resize on the actual editor — source-level review only, no
  native/GPU editor session was run.
- (Merge note: the branch conflicted with #188's test tail in
  test_prefab_live_sync.cpp; resolved as the union of the
  LegacyIdentityCandidates and RestoreSelection coverage.)

## #198 — opt-in programmable MSAA 2x HDR resolve (merged uncompiled)
- The PR's own gate said "not GPU-validated or locally compiled. Do not
  merge until build, DXC shader compilation, validation layers and
  parity checks pass" — merged on owner request anyway.
- (Merge note: two compile breaks fixed in `a06a7bec` — a duplicated
  Msaa2xCustom case label in the AA switch and a nonexistent readFile()
  call in MsaaCustomResolve::Ensure, now the shaders::Read pattern.)
- Post-merge validation: build + DXC + 107/107 ctest green; the
  sponza AA A/B (msaa2x vs msaa2xcustom, benchmarks/msaa_ab.json)
  captured headlessly. **A second defect was found and fixed by exactly
  that gate (`7c611a0d`)**: ApplyHeadlessPostfxOverrides' AA switch
  lacked the Msaa2xCustom case, so scene seeds silently reset the mode
  to Off — the first A/B capture was pixel-identical to AA off. After
  the fix the custom resolve engages at near-bit-exact parity with the
  hardware average resolve (mean 0.001, max 1.0, 0.37% differing).
- **Validation-layer session DONE (2026-11-05)**: App + msaa2xcustom +
  sponza headless bench with the engine's validation layers on — exit 0,
  zero VUID/validation errors, capture produced. Remaining: the
  silhouette/coverage EYEBALL (the A/B captures exist under
  out/msaa-ab*).

## #191 — [Audit 10] WorldRenderer decomposition (partial, config + descriptors)
- Author STATUS: "frame-pass preset policy and pass-descriptor
  configuration are extracted; **orchestration extraction, native
  tests, and GPU parity remain integrator follow-ups.**"
- Integrator validation done post-merge: full build + 107/107 ctest, and
  a BIT-EXACT behavior-preservation proof — the MSAA A/B captures are
  pixel-identical pre/post decomposition (0.000 delta both modes; the
  custom-vs-hardware parity contract still holds at mean 0.001).
- (Merge note: the earlier msaa_custom_resolve.h `program = {}` broke
  the LINUX CI leg only — the documented GCC/ShaderProgram brace
  landmine, MSVC accepts it; fixed in `84180b24` with the paren form +
  the WSL g++ syntax sweep now part of the pre-push ritual for engine
  headers.)
- Still open: the orchestration-extraction stages, and the AGENTS.md
  "god objects are INTENTIONAL" owner note stands — this lands as
  narrow-collaborator extraction, not a file split.

## #199 — GPU mesh-instance particles (merged with tests unrun)
- **Integrator validation DONE (2026-11-05)**: full build + DXC +
  107/107 ctest (the PR's own CPU regressions: mesh ABI, emitter
  compilation, schema compatibility, invalid bins, mixed budgets).
  (Merge note: the prototype-bounds loop indexed PrototypeTable with
  operator[] — fixed to the size()/at() pattern, `f3caa27e`.)
- Remaining: a GPU runtime-rendering smoke with AUTHORED mesh-particle
  content (no scene with mesh emitters exists in the Sponza release)
  and a perf benchmark once such content exists.
- Known v1 limitations (author-declared): diagnostic unlit draw path;
  material-aware PBR and a prototype registration UI are follow-ups.

## #201 — local-first multi-machine scenario correctness records
- **Single-machine validation DONE (2026-11-05)**: tools compile, the
  test_scenario_matrix suite passes, and an end-to-end local matrix run
  (sponza-atrium, snapshot mode) produced a `pass` record under
  out/scenario-matrix-records/.
- Found + pending: the example matrix's extensionless `exe` spelling
  fails on Windows (the perf harness's `[.exe]` tolerance is not
  inherited by the matrix tool) — see the easy-target list below.
- Remaining: real multi-machine GPU runs (needs other machines) and the
  author's follow-up (golden-sequence comparisons + cross-machine
  aggregation — blocked on a baseline policy decision).

## #204 — Unicode corruption guard (checker + CI + AGENTS protocol)
- (Merge note: the checker and its test BOTH carried the literal
  backslash-n generation artifact — the THIRD occurrence of that
  disease: the is_powershell assignment was swallowed into a comment
  (NameError) and two test methods merged into one broken line. Also
  the scan flagged its own fixtures and exited 1 on a clean tree,
  which would have kept the workflow permanently red. Fixed in
  `ea079324`.)
- **Reshaped per owner direction (`4a704ab9`)**: the verification is
  JUST A TEST now — `test_tracked_tree_is_clean` scans the whole
  tracked tree inside the unittest suite (one command = fixtures +
  enforcement), the CI workflow runs exactly that suite, and AGENTS.md
  carries a five-line pointer instead of the 28-line agent checklist
  (AGENTS.md itself is back in the CHECKED set, clean).

## #205 — contaminated-benchmark failure + CPU/GPU/RAM telemetry
- CPU-side validation: tools compile, the perf_harness suite passes.
- **Contamination guard validated LIVE (2026-11-05)**: two bench
  attempts on this dev box were REFUSED at preflight ("other
  3D/compute GPU processes >= 5%") — exactly the designed behavior
  while the GPU is busy. The telemetry RECORD itself needs a quiet GPU
  window on a real machine (the harness output + result.json shape are
  covered by the unit tests).

## Audit series still open (drafts)
- #184 [Audit 03] double-precision transforms (docs-only, unstarted).
- #191 [Audit 10] WorldRenderer decomposition (partial implementation
  on the branch, not merged).

## Legacy engineering backlog (migrated from `todo.txt`)

The following older audit notes, decisions, and deferred engineering work were
kept intact while consolidating tracking into this file. Some historical
"LANDED" status notes reflect their original audit context; recheck current
code before starting an item.

```text
TODO
====

(Proofread backlog — highest value first)

1. `pak.cpp`: TOC/blob `u64` overflow checks, `Payload` entry-provenance;
   the pool-ctor `ThreadPool(0)` hang is guarded now, but the pak sites
   still use `min(count, hc)` — wrap with `max(1u,…)` when touched.
2. `impl.cpp` barrier collection: `continue`-on-missing skips (assert+log
   instead); `%2` SDSM slots vs 3 flights (verify/convert to flight count).
3. Bindless: exhaustion must fail closed (`UINT32_MAX`, not assert-only);
   destroy path is being hardened upstream (`NullBindlessSlotWrites`) —
   coordinate, don't duplicate.
4. `AppFrameContext::gfxLodFilterable` (`std::function`, nullable): null-guard
   at call sites; don't copy per frame.
5. `RenderGraph::NewFrame` pool growth: free/reuse transients, don't
   `AddImage` per frame without free.


(Engine, future) Two-sided materials (CullMode::None): the importer asserts
on doubleSided under --strict while the runtime IGNORES the cooked flag
(mesh.h: "two-sided rendering is no longer supported (the pipeline is
always back-face culled)" — MakePipelineStateForMaterial pins Cull Back).
Content wants it: glass panes already stamp doubleSided=true (gltf.cpp's
stale "pipeline cull None via doubleSided" comment), and thin double-skin
shells (the knight's cloth/shield) suffer under Cull Back (see-through at
open edges/folds/grazing angles) + under the Cull Front shadow depth (the
thin-shell self-shadow acne — see the standing verdict below). Support
shape: un-ignore Material::doubleSided -> MakePipelineStateForMaterial
gains a CullMode::None raster variant for those materials across every
pipeline family (lit/depth/prepass/motion/seed + the direct and indirect
legs), plus a shadow-depth policy for two-sided casters (Cull None + the
existing receiver-side bias, or keep Cull Front and accept the acne), and
the importer's --strict assert drops once the flag is real. The meshlet
cones stay safe either way (facing-mixed -> degenerate -> keep); the
per-triangle homogeneous backface filter flag needs no change.

(Engine, standing decision) Skinned-mesh culling runs CONSERVATIVE by
default: the cone test and the HZB occlusion test are SKIPPED for skinned
draws/clusters (sphere + frustum + contribution only). The fit is verified
honest (TOURMALINE_SKINBOUNDS_DUMP's CPU-reference cross-check agreed
2000/2000) and ~42% of the knight's posed clusters degenerate their cones
anyway — but the fog evidence (a complete depth silhouette while Main
misses meshlets) pinned the visible-chunk drops on the lossy filter
stages, so skinned geometry stays off both until the residual dropper is
isolated (the shipped-alan-wake-2 precedent: cones rebuilt, culling
disabled). HZB skip for skinned has no lever yet — add one if the
occluder leg is ever exonerated. The SkinDump diagnostic stays until the
investigation fully closes (remove in the squash).

(Engine, standing decision) Skinned-mesh self-shadowing on thin
double-skin meshes (the knight's shield): the residual acne is GEOMETRIC,
not a bias bug — shadow depth renders with Cull Front, so on a THIN OPEN
shell (~2mm skins) the stored back skin sits 2mm behind the receiver skin
and grazing sun angles unbound the required detachment. RIDE WITH IT.
Content-side fixes: close the shell rim (solid edge strip) or widen the
skin gap beyond the bias budget (~>1 cm). Engine-side answer if it ever
matters: EVSM (leaks light through tiny overlaps natively).

(Editor, future) The editor's meshlet debug (F4 "Cone-cutoff heat") reads
its own cooked STATIC meshlets — bind-pose cones, no deformed source. The
engine's gfx cull chain rebuilds deformed cones on GPU (GfxSkinBounds +
the runtime GfxConeDebug overlay); the editor preview is the remaining
place a skinned mesh shows bind-pose cone data. Revisit if the editor
ever runs the skinning preview / the gfx filter chain.

(Engine, next-PR) PR07 — THE PLACEMENT SHADOW-VIEWS + OCCLUSION/LOD +
THE PERF/GOLDEN CONTRACT. PR01-PR06 left the chain MAIN-VIEW-ONLY and
FRUSTUM-ONLY: PlacementWorldRuntime records one cull chain + one draw
per tile (viewBudget defaults 1) and the cull verdict is the cooked
sphere-vs-frustum + species distance + constraint-alive
(placement_cull.hlsli; PLACEMENT_CULL_FRUSTUM) — no occlusion, no LOD,
no shadow views. Four scoped gaps, in dependency order:

  - Shadow-view cull policies: the per-(slot, view) machinery is
    ALREADY THERE (the vis array strides [slot * viewBudget + view],
    PlacementVisibleSet is per view, and placement_cull.cs takes
    (renderViews, renderViewIndex)) — what is missing is the POLICY +
    the wiring. Shadow cascades need placed geometry too but should NOT
    pay the main view's cull shape: (a) one shared conservative
    FRUSTUM-ONLY visible set recorded for the union of the shadow
    views (viewBudget 2: main + one shared shadow set), or (b)
    per-cascade sets (viewBudget 1 + cascadeCount). Decide with the
    per-pass timings from the perf scene below, then wire the shadow
    draws into the cascade views' command recording (the renderer's
    shadow-view list joins RecordCullChain).
  - HZB/LOD: every placed instance today draws its BASE proto mesh —
    no LOD tier, no occlusion rejection. Borrow the mesh side's halves:
    the HZB pass + its per-view occlusion walk, and World::SelectLodIndex
    (LOD sibling errors stay MESH-LOCAL — never bake a scene transform
    into the serialized error). Shape: a placement-tier LOD per proto
    (selection needs the instance's world scale + the VIEW's distance —
    a per-view distance band picking the tier in the cull/build pass)
    and OPTIONALLY one HZB sample against the placement sphere.
    A/B both on the perf scene before either becomes load-bearing —
    the skinned-mesh lesson says prove the occluder leg exoneration
    FIRST, pin with a lever, ship conservative by default.
  - Perf scenes: a placement-heavy bench target under the --bench +
    TOURMALINE_CAPTURE contract with the per-pass timings
    (scatter/prefix/cull/drawbuild/draw) in the stderr table — the
    chain needs ONE scene it is tuned against, the role sponza played
    for the octree/volumetrics A/B matrix.
  - Golden captures: the capture + diff protocol as the placement
    regression gate — a golden per PIN-MATRIX (the TOURMALINE_* pin
    set -> one screenshot, re-captured on shader/runtime changes),
    compared with the NOISE-FLOOR contract (identical-source runs
    differ ~11.5k-12k samples / sum ~15.0-15.6k / max ~20 at 4 px
    stride; any honest "unchanged" verdict must sit BELOW it).

(Engine, deferred) Forward+ hierarchical tile-light culling follow-ups
(TOURMALINE_FP_MASKS, default ON, validated: the invariant suite + a
byte-identical sponza A/B capture; under DS the F+ light leg stays
linear by design — the DS lists own the lights, the decal/probe/volume
legs still run):

  - GPU-side phase-1: the sort + DsBuildZBins + DsBuildTileMasks run on
    the CPU every frame (~0.15 ms at 96 lights / 1080p). If heavy-dynamic
    scenes push that past ~0.5 ms, move the zBin/mask BUILD to compute
    (the paper does phase 1 on GPU; the light sort itself can stay
    CPU-side — the builder order is the zBin index space).
  - Occupancy batching: forwardplus.cs.hlsl still dispatches [1,1,1]
    workgroups. Batching 8x8 tiles per workgroup (64 lanes) is
    mechanical and should pay on the depth-preload + candidate walk;
    keep the [1,1,1] shape as the A/B reference.
  - Compact per-tile arena lists: deliberately NOT ported — the
    fixed-stride rows with count-gated consumers ARE the guide's
    "fixed capacity first version". Revisit only if a profile pins
    list-read bandwidth in the lit body.
  - DS-off caveat: m_dsEnabled defaults TRUE, so the masks pay on the
    DS-off path (H toggle / TOURMALINE_DS=0) until DS flips default-off.

(Entity model, next) THE PER-DRAW PALETTE TABLE — one rendered POSE per
cluster today (World::Update flushes the FIRST animator per target
cluster; every animator's STATE is already independent + exact). The
renderer generalization: a palette per DRAW, not per cluster — the
skinning pass grows a palette table (per-entity ranges keyed by draw),
SkinPush gains the per-draw palette index, and GFX_DRAW_SKINNED fetches
its own palette. Unblocks N knight entities sharing one cluster with
independent poses. See docs/entities_and_animators.md (v1 limits).

(Entity model, next) THE ASSET-MANAGER SURFACE — the typed-key MESH lane
is LANDED (AssetManager::LoadMesh over the project MeshKeys +
SpawnMesh(handle)); the remaining halves:

  - the AnimationClipKey + SkeletonKey CATALOG BUCKETS (0x5xxx ranges:
    .anim classification in recipes, the asset-db kind, the bucket
    emission, the [ids] rows — the .anim blobs already embed as
    Texture-kind at VFS paths so ReadAsset serves the text;
    LoadTourAnimText parses). Clips today pass as loaded
    touranim::Clip values.
  - one skeleton per cluster (the importer's first-skin-wins contract)
    — a mesh-keyed skeleton table is the deeper unblock for
    multi-character clusters.

(Audio, future) Spatial audio v1 scope upgrades: cones/occlusion,
emitter orientation (nodes are omnidirectional today), compressed +
streaming formats (WAV/SDL only), more than the first autoplay slot,
and the audio layer's per-process pak-image cost note (the shared
apphost->game runtime seam — RunPakApp handing its preloaded backends
to the AudioRuntime's ProjectRuntime).

(Namespace/RHI boundary audit, REMAINING — the stages LANDED so far
live on feature/namespace-rhi-boundary; work in order:) THE tour::/
smvk:: SPLIT. TARGET: tour:: = the public engine API (apps, scenes,
assets, camera, world, audio, gi, placement, mem, profiler); smvk:: =
the RHI ONLY; smvk::detail::vulkan = the private impl. Remaining:

  4. THE PRIMARY MIGRATION (engine-owned definitions OUT of smvk:: ->
     tour::: App/app.h, App/world.h, App/cluster.h -> tour::(scene);
     Asset/pak.h + Asset/tour.h's smvk::tour -> tour::asset;
     smvk::audio/gi/placement/mem/profiler -> tour::*) — compat
     using-aliases keep the tree compiling; retire as call sites
     migrate. The giants' order: placement (13 headers, same shape
     as gi), the global-namespace cleanup (NodeId/scene tables/
     payloads/Camera/Input -> tour::(scene)), App/world/cluster+
     pak+tour.h.
      LANDED (finding-3 declaration moves, each with its R7 compat
      record): the engine shader library (Utils/shader_library.h +
      src/Utils/shader_library.cpp) -> tour::shaders, alias
      smvk::shaders; the geometry streaming policy layer
      (Renderer/geometry_streamer.h) -> tour, per-name aliases (the
      block is std-only — the cleanest move); the texture streaming
      family (Renderer/texture_streamer.h + .cpp) -> tour, RHI types
      spelled smvk::-qualified, no broad import; mesh.h's material data
      model trio (MaterialValues/ShadingModel/AlphaMode) -> tour with
      smvk re-exports; the gfx cull/filter family
      (Renderer/gfx_filter.h + gfx_filter.cpp) -> tour::gfx, namespace
      alias smvk::gfx, the header FULLY QUALIFIED after the broad-import
      form's transitive lookup proved ambiguous (tour::gfx::Cluster vs
      tour::Cluster for using-namespace-gfx consumers — the recorded
      lesson: qualify in headers, TU-local using-directives in .cpps).
      LANDED (the two giants): (a) mesh.h's whole renderer DATA MODEL
      -> tour:: (Material/StaticMesh/StaticMeshUpload/ImportStaticVertex,
      the packed-vertex + terrain/skin streams, ShaderLod*,
      MaterialCreateInfo, the material flags/word helpers — 35 smvk
      re-export rows; mesh.cpp follows) — the vertex-INPUT contract
      (VertexLayoutId + the layout constants + GetVertexLayoutDesc,
      defined against smvk.h's VertexLayoutDesc and consumed by the
      Vulkan backend) deliberately STAYS smvk::; the three cross-file
      `struct StaticMeshUpload;` fwd decls (meshlet.h/lightmap.h/
      gfx_filter.h) became using-declarations so they cannot collide
      with mesh.h's compat block. (b) THE GLOBAL-NAMESPACE CLEANUP IS
      COMPLETE: bvh.h -> tour + the global bridge, dxc_integration.h
      -> tour::dxc + bridge, and the stale GLOBAL `struct Scene;` fwd
      decls in tour.h/touranim.h (a ::Scene DISTINCT from
      tour::scene::Scene — a C2874 landmine against scene.h's global
      alias) are gone; NO engine type is declared at global scope
      anymore (NodeId/scene tables/payloads/Camera/Input/BVH/DXC all
      live in tour::(scene|dxc|...), bridged by the audited global
      re-export rows). R9 gates it: new global declarations fail
      outright, the bridges are shrink-only per-file counts.
      NEXT in the same shape: placement's 13 headers + the remaining
      smvk::-owned asset family (pak_pager, appmanifest, vfs, the
      node schemas) — the placement headers already carry their
      compat blocks; finishing is call-site spelling + alias
      retirement.
  5. P0 INTERFACE REDESIGNS (the real boundary work):
     - smvk::App exposes GetResourceManager/GetSwapchain/
       GetRendergraph + stores RHI members -> tour::App owns the app;
       the concrete rendering context moves behind an impl/bridge.
     - engine::ElementFrame carries smvk::Framebuffer*/Extent2D ->
       a tour::RenderFrame hiding swapchain + command ownership.
     - GfxRenderer publicly inherits smvk::IFrameRenderer + exposes
       shader programs/handles; WorldRenderer.h ~4000 lines ->
       a compact public renderer API vs private pass state.
     - engine/core.h: the umbrella header - split it.
     Whole-tree sequence note: impl.cpp's App::Initialize BOOTS the
     whole renderer — moving that behind a bridge is a rearchitecture
     of the 14k-line backend file, sequenced first among these.
  6. P1s: WorldField's smvk::Format/ImageHandle/ImageRegion3D
     exposure; dlss.cpp's direct VkFormat/VkImage/
     VkPhysicalDevice use (Streamline adaptation -> a backend file);
     RHI's SetImageViewVkView opaque-pointer API + swapchain
     GetImpl()/GetVulkanOpaque() -> internal backend accessors.
  7. Downstream: Host.h's callback mixes smvk::App/World +
     engine::WorldRenderer; editor_app.h includes everything;
     01_triangle is a DELIBERATE low-level RHI sample (opt in
     explicitly); 04_project_game should demonstrate the
     engine-only surface.
     LANDED (the finding-7 consumer migration, app + editor): the
     audit-named sites + the whole migratable surface moved to
     canonical spellings - Host.h's extraElement callback
     (tour::App/tour::World*), ProjectScenes' signatures (the
     smvk::Cluster fwd decl that would collide with tour.h's compat
     using-decl is gone; clusterLoader takes tour::World/
     tour::Cluster/tour::asset::PakReader), editor_app.h's
     EditorApp base (tour::App), PakApp's LOD-page-source install
     (tour::SetLodPageSource), AudioRuntime/AssetStore/
     AssetManager/PakWorldLoader (tour::World/Cluster/
     TextureStreamer/mem), the smvk::shaders -> tour::shaders call
     sites (DeployCheck, main, matball, editor_render, editor main),
     and the editor TUs' Cluster/World/mem spellings (~130 sites).
     Remaining downstream: the engine-internal TUs (WorldRenderer/
     impl.cpp spellings retire with the 8.5 interface redesigns);
     tools + samples + tests keep the alias spellings until their
     own passes; 01_triangle stays DELIBERATE.
  8. Deep follow-ups (block only on real need): the three-target
     build split (tour_engine + smvk_rhi + smvk_vulkan — blocked on
     impl.cpp's App coupling), the null/mock backend test, the
     AST SIGNATURE-WALKING namespace scanner (the compile-level CI
     battery is R1-R9 + header smoke: R6/R7/R8/R9 LANDED as the
     baseline-ratchet form - R6 is now ABSOLUTE (the remaining-changes
     item 1 landed: ZERO broad using-namespace-
     ::smvk imports in public headers; the last ten removed with their
     RHI names qualified - the impl TUs that still spell them
     unqualified carry the documented TU-local using-directive
     (impl.cpp, pak.cpp, mesh.cpp, gfx_filter.cpp) and the R6 baseline
     is EMPTY, any new public-header import fails outright. Recorded
     lesson: placement_pass.h silently leaned on placement_terrain.h's
     directive through the shared enclosing namespace), R7 confines the
     smvk::-compat re-export blocks to the audited per-file counts,
     R8 freezes the RHI implementation-state exposure shapes
     (App's getters/LinearPtr members, ElementFrame's Framebuffer*,
     gfx::State, the raw RHI pointer types), R9 gates the GLOBAL
     namespace (new global engine-type declarations fail outright;
     the global re-export bridges: input.h/scene.h/scene_tables.h/
     scene_payloads.h/scene_node_id.h/camera.h/envzones.h/bvh.h/
     dxc_integration.h, shrink-only counts), and R2/R4/R5 now see
     QUOTED include paths while R5 walks past Renderer/. Every
     baseline is exact BOTH ways: growth AND silent staling fail
     (retiring compat surface requires deleting its baseline row in
     the same commit). What still needs the AST walker: signature-
     walking exported-API analysis (fields/params/returns/inheritance
     of RHI types beyond R8's regex vocabulary; the declaration-level
     finding-3 work above is DONE by hand), and the bridge-call-site
     retirement (mechanical, endless, opportunistic).

(Content architecture, LANDED + the deferred tail — the content-session
branch) The consolidated content architecture landed: tour::asset::
ContentRegistry (the ONE resolution authority — id and path forms
converge), ProjectRuntime as the SESSION over it, per-pak "#ids"
identity tables (the build appends them post-cook), the multi-pak/
--scene .tapp-free lanes, the overlay no longer a content source on
any runtime path, app::RunGame (the run<Game> boundary, the FATAL
compiled-catalog gate, composed element lists), pakIndex gone from
the catalog (identity is id+kind+path), and appmanifest 2 (the .tapp
is a six-line RELEASE POINTER: entry + [paks]; the [ids] mirror died
loudly). DEFERRED, deliberately:

1. The SHADER LIBRARY keeps its own OverlayVfs (a protected-domain
   decision: the dev override tree + engine-shaders.pak + MountPak mod
   packages, precedence -20..0 — shader paths never collide with the
   catalog's resolution domain, so the overlay is not competing with
   the registry's authority. Migrating it onto the provider mechanism
   wants a session available at engine-init time; block on real need).
2. MeshPlanRef still keys by MOUNT INDEX (project_plans.h's pakIndex
   → the manifest's [paks] order). It is a PRIVATE contract, validated
   fail-closed (AttachCompiledPlans folds the deployed layouts; a
   count or fold mismatch is a loud abort, never a dynamic fallback) —
   the compiled plans never claimed to be identity. The
   provider-relative hint shape (resolve the provider first, validate
   kind + layout, then the direct slot) is the follow-up if modded
   content ever needs to invalidate plans without recompiling.
3. LIVE MOD TOGGLING: providers toggle by name with generation bumps
   (between-launches semantics; loaded resources never mutate), but
   the coordinator that applies a toggle to a LIVE scene at a safe
   boundary (scene replacement, in-use resource reloads) is not built
   — the ownership registry (ProjectScenes) + the provider generation
   are the machinery that would drive it.
4. INSTALLER-LEVEL RELEASE MANAGEMENT: appmanifest 2 IS the atomic
   pointer to a complete generation (entry + [paks], written last,
   renamed over atomically) — the release-root/pointer-file
   indirection (an installer's current-release directory or an atomic
   pointer file) composes on top of exactly that shape when
   multi-generation deployments need it.

Standing pre-merge gate for the branch: the golden/dev-GPU pass
(bistro/sponza captures + the sample's visual launch) — the headless
suite + the live smokes on the dev machine covered everything else.
```
