# Maya rig tooling

MEL scripts for the knight rig's constraint-baking workflow. Run with
Maya 2022 batch (`mayabatch.exe -file <script>`); the MEL path avoids the
Arnold/PySide2 Python 3.12 vs 3.7 shiboken crash that `mayapy.exe` hits.

## The problem

The knight rig has two joint layers:

- **FK/control joints** (`hipSwinger_M`, ...) — what the animation drives.
- **deform joints** (`unrealhipSwinger_M`, `unreal*Flap*`, ...) — what the
  mesh is skinned to, PARENT-CONSTRAINED to the FK joints.

A naive export bakes the deform joints' **parked rest pose** (`.bindPose` /
the `dagPose` matrix) instead of their **evaluated constrained pose**. The
glTF `inverseBindMatrices` then carry the parked orientation/position, so
the skinned region (e.g. the hip) renders offset while still following the
animation — "badly positioned but follows animation perfectly".

## Fix

1. `bake_constraints.mel` — bakes the constraint-driven animation into
   keyframes (`bakeResults`) and resets the bind poses to the evaluated
   pose at frame 1 (`dagPose -reset`).
2. Re-export: Maya → FBX → Blender → glTF → `Import.exe`.

`query_pose.mel` is the diagnostic companion — it dumps the skin clusters,
the `dagPose` node membership, and the bind vs constrained pose of a joint.

## Notes

- The scene has two rigs: the main rig (`root`/`pelvis`/`spine_01`, 84
  influences, `bindPose8`) and a `_M` rig (`Root_M`/`Spine1_M`, 86
  influences, `bindPose1-4`). Only the main rig feeds the exported glTF.
  `bake_constraints.mel` skips empty poses (`dagPose -q -mn == 0`) and
  resets each non-empty pose individually.
