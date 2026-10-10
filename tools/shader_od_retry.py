#!/usr/bin/env python3
"""CI helper: retry the HLSL_FORCE_O3-pinned shaders with -Od codegen.

The -Od SPIR-V of gfxfilter/batchcull/drawcull AVs nvgpucomp64.dll inside
vkCreateComputePipelines on affected drivers, so Debug pins them to -O3
(see cmake/CompileShaders.cmake). This script recompiles just those files
with the Debug -Od flags and reports whether the driver bug still bites:

  python tools/shader_od_retry.py [--dxc path/to/dxc.exe]

Exit 0 = all pinned shaders compiled with -Od (re-test the pipeline
compile too: configure with -DVULKANTOY_SHADER_OD_RETRY=ON and run the
affected benches). Exit 1 = DXC itself failed. The driver AV happens later
at vkCreateComputePipelines, so a DXC exit-0 here is necessary but not
sufficient to unpin — read the header comment before deleting the pin.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED = ["gfxfilter.cs.hlsl", "gfxbatchcull.cs.hlsl", "gfxdrawcull.cs.hlsl"]

COMMON = [
    "-spirv",
    "-fspv-target-env=vulkan1.3",
    "-fvk-use-dx-layout",
    "-Od", "-Zi", "-Qembed_debug", "-fspv-debug=vulkan",
]


def find_dxc(explicit: str | None) -> str:
    if explicit:
        return explicit
    sdk = os.environ.get("VULKAN_SDK", "")
    cand = str(Path(sdk) / "Bin" / "dxc.exe") if sdk else ""
    if cand and Path(cand).exists():
        return cand
    return "dxc.exe"


def main() -> int:
    dxc = find_dxc(sys.argv[1] if len(sys.argv) > 1 else None)
    failures = 0
    for name in PINNED:
        srcs = list((ROOT / "engine" / "shaders").rglob(name))
        if not srcs:
            print(f"SKIP {name}: not found under engine/shaders")
            continue
        src = srcs[0]
        out = Path(os.environ.get("TEMP", ".")) / f"{name}.od-retry.spv"
        cmd = [dxc, "-T", "cs_6_7", "-E", "mainCS", *COMMON,
               "-I", str(ROOT / "engine" / "shaders"),
               "-Fo", str(out), str(src)]
        print(f"--- {name}\n{' '.join(cmd)}")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        except Exception as e:  # noqa: BLE001 - diagnostic script
            print(f"FAIL {name}: {e}")
            failures += 1
            continue
        print(r.stdout[-2000:] if r.stdout else "(no stdout)")
        if r.returncode != 0:
            print(f"FAIL {name}: exit={r.returncode}\n{r.stderr[-4000:]}")
            failures += 1
        else:
            size = out.stat().st_size if out.exists() else 0
            print(f"OK {name}: {size} bytes -> {out}")
    if failures:
        print(f"RESULT: {failures} shader(s) failed -Od compile; keep the -O3 pin.")
        return 1
    print("RESULT: all pinned shaders compiled with -Od. "
          "Next: configure -DVULKANTOY_SHADER_OD_RETRY=ON and run benches "
          "to confirm vkCreateComputePipelines no longer AVs before unpinning.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
