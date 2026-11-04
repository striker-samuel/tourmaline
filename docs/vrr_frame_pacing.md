# Variable refresh rate (VRR) frame pacing

The Vulkan swapchain supports FIFO, mailbox and immediate present modes. The interactive
host also has a CPU frame limiter, separate from the swapchain's presentation mode.

For a VRR-capable display, enable VRR in the monitor, OS and GPU driver and launch the
viewer with:

```sh
App --project game.tapp --vrr --vsync on
```

`--vrr` detects the current SDL display refresh rate and automatically limits the
interactive frame rate to approximately **2 FPS below** the rounded refresh rate
(e.g. **142 FPS on 144 Hz**, 58 FPS on 59.94/60 Hz). The cap is recomputed if the
window moves to a different display or its active refresh rate changes.
A missing/invalid refresh rate disables the automatic cap rather than inventing one.
This is a heuristic, not VRR capability detection or a guarantee of low latency.

## Controls and precedence

- `--vsync on`: request Vulkan FIFO presentation (the default). Prevents tearing
  when supported, including when frame rates leave the VRR operating range.
- `--vsync off`: request immediate presentation (may tear).
- `--present fifo|mailbox|immediate`: choose the Vulkan present mode directly;
  cannot be combined with `--vsync`. Requested present modes may be substituted
  by the Vulkan backend when unsupported by the surface.
- `--vrr`: automatically set the CPU FPS limit below the display refresh rate.
  It does not enable VRR at the operating-system/driver/display level, and works
  with any present mode; FIFO is the recommended choice.
- `--max-fps N`: manual interactive cap, taking precedence over `--vrr`.
  Use this to tune latency, adjust the safety margin, or cap below a VRR minimum.
- `--bench`: bypasses the CPU limiter (including the VRR cap); keeps its
  existing immediate-presentation default unless a present mode is specified.

Both the CPU cap and FIFO presentation can affect latency. The cap reduces the
likelihood of touching the fixed-refresh ceiling, but the graphics driver,
compositor, VRR range, GPU load, and other latency mechanisms still matter.
No Vulkan API can guarantee that a display is actively in VRR mode.

## Validation on hardware

Test a VRR-enabled display at 60, 120, 144 and 240 Hz where available, both
windowed and exclusive/fullscreen-equivalent modes supported by the platform.
Confirm the display's refresh-rate overlay responds to frame-rate changes,
that capped rates stay inside its VRR range, and that no tearing occurs with
FIFO presentation. Verify display migration, dynamic refresh changes,
minimization/restoration, heavy GPU workloads, manual caps, and uncapped benches.
CPU-only unit tests cover the FPS target selection but cannot prove VRR behavior.
