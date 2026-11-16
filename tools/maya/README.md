# Maya rig tooling

MEL scripts for inspecting and re-binding the knight rig's deform-joint
constraints. Run with Maya 2022 batch (`mayabatch.exe -file <script>`); the
MEL path avoids the Arnold/PySide2 Python 3.12 vs 3.7 shiboken crash that
`mayapy.exe` hits.

## The rig

The knight rig has two joint layers:

- **FK/control joints** (`hipSwinger_M`, ...) — what the animation drives.
- **deform joints** (`unrealhipSwinger_M`, `unreal*Flap*`, ...) — what the
  mesh is skinned to, PARENT-CONSTRAINED to the FK joints.

## A content-bug hypothesis that was disproven

The knight's "badly positioned hip" was originally suspected to be a content
export bug: a naive export would bake the deform joints' **parked rest pose**
(`.bindPose` / the `dagPose` matrix) into the glTF `inverseBindMatrices`
instead of their **evaluated constrained pose**, leaving the hip region
offset while still following the animation.

That hypothesis was **wrong**. The shipped glTF is internally consistent:
for every joint, `nodeWorldMatrix * inverseBindMatrix == identity` (to
~1e-7), and the deform joints' node TRS already encodes the *constrained*
(evaluated) pose (~1.1 cm from the pelvis), not the parked pose (~7 cm). The
Maya→glTF conversion is a single left multiplication `gltf = C · maya`
(`C = gltf_pelvis_bind · inv(maya_pelvis)`, exact to ~5e-18).

The hip issue was an **engine-side** skinned-mesh rendering bug, fixed by the
renderer changes (skinned caster shadow tail, palette/range upload bounds,
work-select push-constant layout, weld winding) — not a content problem. Do
**not** patch the glTF `inverseBindMatrices` for this; it is already correct.

## Scripts

- `query_pose.mel` — diagnostic: dumps the skin clusters, the `dagPose` node
  membership, and the bind vs constrained pose of a joint.
- `bake_constraints.mel` — bakes the constraint-driven animation into
  keyframes (`bakeResults`) and resets bind poses (`dagPose -reset`). Only
  needed if a future re-export does not evaluate the constraints itself.

## Notes

- The scene has two rigs: the main rig (`root`/`pelvis`/`spine_01`, 84
  influences, `bindPose8`) and a `_M` rig (`Root_M`/`Spine1_M`, 86
  influences, `bindPose1-4`). Only the main rig feeds the exported glTF.
  `bake_constraints.mel` skips empty poses (`dagPose -q -mn == 0`) and
  resets each non-empty pose individually.
- The joint→skin-index mapping: `pelvis`=0, `unrealhipSwinger_M`=71,
  `unrealfrontFlap_01_R`=72, `unrealbackFlap_01_R`=73,
  `unrealbackFlap_01_L`=75, `unrealfrontFlap_01_L`=77.
