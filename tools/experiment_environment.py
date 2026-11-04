#!/usr/bin/env python3
"""Best-effort reproducibility metadata for local shader/benchmark experiments.

No network or shell execution. Missing probing tools are represented as null.
"""
from __future__ import annotations

import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


def probe_command(argv, timeout=5):
    try:
        proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace", timeout=timeout, check=False)
        return proc.stdout.strip()[:8192] if proc.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired, ValueError):
        return None


def windows_build_info():
    if sys.platform != "win32":
        return None
    version, _csd, _ptype, _ = platform.win32_ver()
    win = sys.getwindowsversion()
    # Windows 11 often returns kernel 10.0; don't guess the product name.
    return {"kernel_version": version or None, "major": win.major,
            "minor": win.minor, "build": win.build,
            "service_pack": win.service_pack or None}


def gpu_info():
    """Prefer Windows CIM for all vendors; append optional NVIDIA-SMI details."""
    controllers = []
    if sys.platform == "win32":
        ps = shutil.which("powershell.exe") or shutil.which("powershell")
        if ps:
            cmd = ("Get-CimInstance Win32_VideoController | "
                   "Select-Object Name,DriverVersion,PNPDeviceID,VideoProcessor | "
                   "ConvertTo-Json -Compress -Depth 3")
            data = probe_command([ps, "-NoProfile", "-NonInteractive", "-Command", cmd])
            if data:
                try:
                    import json
                    value = json.loads(data)
                    for row in value if isinstance(value, list) else [value]:
                        if isinstance(row, dict):
                            controllers.append({
                                "name": row.get("Name"),
                                "driver_version": row.get("DriverVersion"),
                                "pnp_device_id": row.get("PNPDeviceID"),
                                "video_processor": row.get("VideoProcessor"),
                            })
                except ValueError:
                    pass
    nvidia = None
    executable = shutil.which("nvidia-smi")
    if executable:
        output = probe_command([executable, "--query-gpu=name,driver_version,vbios_version",
                                "--format=csv,noheader,nounits"])
        if output:
            nvidia = []
            for line in output.splitlines():
                parts = [part.strip() for part in line.split(",")]
                if len(parts) == 3:
                    nvidia.append(dict(zip(("name", "driver_version", "vbios_version"), parts)))
    return {"windows_adapters": controllers, "nvidia_smi": nvidia}


def find_dxc(explicit=None):
    if explicit:
        return str(Path(explicit).expanduser().resolve())
    root = os.environ.get("VULKAN_SDK")
    if root:
        for path in (Path(root) / "Bin/dxc.exe", Path(root) / "bin/dxc"):
            if path.is_file():
                return str(path.resolve())
    local = Path(__file__).resolve().parents[1] / "external/dxc/bin/dxc"
    if local.is_file():
        return str(local.resolve())
    return shutil.which("dxc")


def compiler_info(dxc=None):
    path = find_dxc(dxc)
    # DXC prints version info in -help on common builds. Capture output
    # without interpreting a different compiler as DXC.
    text = probe_command([path, "-help"]) if path else None
    lines = text.splitlines() if text else []
    version_lines = [line.strip() for line in lines
                     if re.search(r"\b(version|dxc|dxcompiler|clang)\b", line, re.IGNORECASE)]
    return {"path": path, "version_text": "\n".join(version_lines[:8])[:1024] if text else None}


def build_info(exe):
    exe = Path(exe).resolve()
    cache = exe.parent / "CMakeCache.txt"
    result = {"cmake_cache_present": cache.is_file(), "build_type": None,
              "shader_debug": None, "shader_od_retry": None}
    if cache.is_file():
        try:
            text = cache.read_text(encoding="utf-8", errors="replace")
            for key, target in (("CMAKE_BUILD_TYPE", "build_type"),
                                ("TOURMALINE_SHADER_DEBUG", "shader_debug"),
                                ("TOURMALINE_SHADER_OD_RETRY", "shader_od_retry")):
                match = re.search(r"^" + key + r":[A-Z]+=(.*)$", text, re.MULTILINE)
                if match:
                    result[target] = match.group(1).strip()
        except OSError:
            pass
    return result


def collect_environment(exe, dxc=None):
    return {"schema_version": 1,
            "os": {"system": platform.system(), "release": platform.release(),
                   "version": platform.version(), "machine": platform.machine(),
                   "windows": windows_build_info()},
            "gpu": gpu_info(), "dxc": compiler_info(dxc),
            "build": build_info(exe)}
