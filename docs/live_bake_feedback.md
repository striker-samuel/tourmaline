# Live bake feedback

The editor's **Cook to .pak** (including `--lightmap on`) and **Bake GI**
operations run the linked-in tool libraries on the worker lane. Their
`tool::out()`/`tool::err()` line sinks tee every completed line to:

- The editor Console and the task-window log (bounded to its recent lines).
- A **flushed, persistent per-job text log** at
  `<project>/.tourmaline/logs/cook-<timestamp>.log` or
  `bake-gi-<timestamp>.log`. When there is no project root, the logs
  use the system temporary directory under `tourmaline/logs/`.

The task window shows the log path with a **Copy path** action. Writes
are flushed at each line, allowing `tail -f` / `Get-Content -Wait`
while the bake is running. stderr lines are prefixed `[stderr]`.
A failed log open produces a warning but does not fail the bake.

## Progress display

The task window and editor status bar show counts and a fraction for the
**current measurable phase** when the baker emits
`lightmap bake: N/M chunks` or `givolume bake: N/M probes` lines.
Chart packing, UV preparation, seam stitching, compression, and stage
transitions show a descriptive, indeterminate stage instead. The progress
bar **does not claim** that a chart or probe phase is a percentage of the
whole cook: later stages may reset it.

The underlying bakers currently emit lightmap progress at approximately
two-second checkpoints **between chunks or groups**; a single expensive
group can still be silent until it completes. Bake GI volume progress
comes from probe batches.

## Why pause/resume is deferred

These are **in-process** operations, not suspendable child processes.
A safe pause needs a cooperative control token at the baker's
thread-pool work checkpoints, with careful shutdown and potentially
checkpointed serialized state for resumption after restart. Suspending
an arbitrary worker thread can deadlock the pool and may leave a partial
atlas or GI binding. No pause/resume button is offered until that
contract is implemented.

## Verification

`EditorBakeFeedbackTests` is a headless test for the progress decoder
and live-readable, stderr-tagged log file. Manual verification should
cover a real lightmap cook, a separate GI volume bake, a failing tool,
and a backgrounded task (including log-file tailing during the run).
