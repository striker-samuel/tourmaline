"""Compile and exercise the shipping-safe C++ GPU memory warning state machine."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r"""
#include <Engine/RHI/gpu_memory_spill.h>
#include <cassert>
using namespace smvk;
static GpuMemoryPressureSample sample(double fraction, unsigned nonlocalMiB=0) {
    return {static_cast<uint64_t>(fraction * 1000), 1000,
            static_cast<uint64_t>(nonlocalMiB) * 1024 * 1024, true};
}
int main() {
    GpuMemorySpillDetector d;
    assert(d.Update({}, 1).state == GpuMemoryWarning::Unavailable);
    assert(d.Update(sample(.70, 100), 1).state == GpuMemoryWarning::Healthy);
    assert(d.Update(sample(.86, 100), 1).state == GpuMemoryWarning::Healthy);
    assert(d.Update(sample(.86, 100), 1).state == GpuMemoryWarning::Pressure);
    // Existing nonlocal allocations should not be misidentified as eviction.
    assert(d.Update(sample(.96, 100), 1).state == GpuMemoryWarning::Critical);
    auto spill = d.Update(sample(.96, 200), 1);
    assert(spill.state == GpuMemoryWarning::SpillSuspected);
    assert(spill.nonlocalGrowthBytes == 100ull * 1024 * 1024);
    assert(!d.Update(sample(.96, 200), 1).changed);
    assert(d.Update(sample(.96, 100), 1).state == GpuMemoryWarning::Critical);
    for (int i=0; i<4; ++i)
        assert(d.Update(sample(.75, 100), 1).state == GpuMemoryWarning::Critical);
    assert(d.Update(sample(.75, 100), 1).state == GpuMemoryWarning::Healthy);
    d.Reset();
    assert(d.Update(sample(.95, 0), 1).state == GpuMemoryWarning::Critical);
    // Unsupported/unified memory must not report zero-budget as healthy.
    assert(d.Update({}, 1).state == GpuMemoryWarning::Unavailable);
}
"""

class GpuMemorySpillTests(unittest.TestCase):
    def test_cpp_detector(self):
        compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "main.cpp"
            exe = Path(tmp) / "gpu_memory_test"
            src.write_text(SOURCE, encoding="utf-8")
            subprocess.run([compiler, "-std=c++20", "-I", str(ROOT / "engine/include"),
                            str(src), "-o", str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)
