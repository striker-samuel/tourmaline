"""Best-effort CPU, GPU and RAM cleanliness gate for measured benchmarks.

Uses only OS tools. Unsupported telemetry is UNKNOWN, never silently clean.
"""
import json
import os
import re
import shutil
import subprocess
import time

GPU_BUSY_PERCENT = 5.0
CPU_BUSY_PERCENT = 20.0
RAM_USED_PERCENT = 90.0


def parse_nvidia_pmon(output, own_pid=None):
    """Parse nvidia-smi pmon. The '-' token denotes unavailable utilization."""
    processes = []
    unknown = False
    observed = False
    for line in output.splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) < 7 or not fields[0].isdigit():
            continue
        observed = True
        pid, kind = fields[1:3]
        if not pid.isdigit() or int(pid) == own_pid:
            continue
        if "G" not in kind and "C" not in kind:
            continue
        try:
            usage = max(float(fields[3]), float(fields[4]))
        except ValueError:
            unknown = True
            continue
        if usage >= GPU_BUSY_PERCENT:
            processes.append({"pid": int(pid), "gpu": fields[0],
                              "name": fields[7] if len(fields) > 7 else "unknown",
                              "utilization_percent": usage})
    return processes, unknown or not observed


def parse_windows_gpu(output, own_pid=None):
    """Identify busy WDDM 3D/compute GPU Engine instances, including browsers."""
    records = json.loads(output)
    if isinstance(records, dict):
        records = [records]
    processes = []
    for row in records:
        match = re.search(r"pid_(\d+).*?engtype_([^\\)]+)", row["path"].lower())
        if not match:
            continue
        pid, engine = int(match[1]), match[2]
        if pid == own_pid or engine not in ("3d", "compute", "cuda", "graphics"):
            continue
        value = float(row["value"])
        if value >= GPU_BUSY_PERCENT:
            processes.append({"pid": pid, "engine": engine,
                              "utilization_percent": value})
    return processes


def _run(command):
    proc = subprocess.run(command, capture_output=True, text=True, errors="replace",
                          timeout=15, check=False)
    if proc.returncode:
        raise RuntimeError((proc.stderr or proc.stdout).strip()[:300]
                           or "telemetry command failed")
    return proc.stdout


def _linux_cpu_snapshot():
    with open("/proc/stat", encoding="ascii") as stream:
        fields = [int(x) for x in stream.readline().split()[1:]]
    idle = fields[3] + fields[4]
    return sum(fields), idle


def _linux_memory():
    values = {}
    with open("/proc/meminfo", encoding="ascii") as stream:
        for line in stream:
            key, _, amount = line.partition(":")
            if key in ("MemTotal", "MemAvailable"):
                values[key] = int(amount.split()[0]) * 1024
    return values["MemTotal"], values["MemAvailable"]


def sample_environment():
    """Return a machine-readable snapshot with status clean, busy or unknown."""
    snapshot = {"status": "clean", "cpu_percent": None, "ram_used_percent": None,
                "ram_total_bytes": None, "ram_available_bytes": None,
                "gpu_processes": [], "reasons": []}

    def unknown(message):
        snapshot["reasons"].append("telemetry unavailable: " + str(message))
        snapshot["status"] = "unknown"

    def busy(message):
        snapshot["reasons"].append(message)
        snapshot["status"] = "busy"

    if os.name == "nt":
        powershell = shutil.which("powershell") or shutil.which("pwsh")
        if not powershell:
            unknown("PowerShell not found")
            return snapshot
        script = (
            "$ErrorActionPreference='Stop'; "
            "$cpu=(Get-Counter '\\Processor(_Total)\\% Processor Time').CounterSamples[0].CookedValue; "
            "$os=Get-CimInstance Win32_OperatingSystem; "
            "$gpu=@((Get-Counter '\\GPU Engine(*)\\Utilization Percentage').CounterSamples "
            "| ForEach-Object { @{path=$_.Path;value=$_.CookedValue} }); "
            "@{cpu=$cpu;total=[double]$os.TotalVisibleMemorySize*1024;"
            "available=[double]$os.FreePhysicalMemory*1024;gpu=$gpu} "
            "| ConvertTo-Json -Depth 5 -Compress"
        )
        try:
            data = json.loads(_run([powershell, "-NoProfile", "-NonInteractive",
                                    "-Command", script]))
            snapshot["cpu_percent"] = float(data["cpu"])
            total, available = float(data["total"]), float(data["available"])
            snapshot["gpu_processes"] = parse_windows_gpu(
                json.dumps(data["gpu"]), os.getpid())
            if not data["gpu"]:
                unknown("Windows GPU Engine counters returned no samples")
        except (OSError, ValueError, KeyError, TypeError, RuntimeError,
                subprocess.TimeoutExpired) as exc:
            unknown(exc)
            return snapshot
    else:
        try:
            start_total, start_idle = _linux_cpu_snapshot()
            time.sleep(0.2)
            end_total, end_idle = _linux_cpu_snapshot()
            delta = end_total - start_total
            if delta <= 0:
                raise ValueError("CPU counters did not advance")
            snapshot["cpu_percent"] = 100 * (1 - (end_idle - start_idle) / delta)
            total, available = _linux_memory()
        except (OSError, KeyError, ValueError) as exc:
            unknown(exc)
            return snapshot
        smi = shutil.which("nvidia-smi")
        if not smi:
            unknown("nvidia-smi not installed (unsupported GPU telemetry backend)")
        else:
            try:
                output = _run([smi, "pmon", "-c", "1"])
                offenders, uncertain = parse_nvidia_pmon(output, os.getpid())
                snapshot["gpu_processes"] = offenders
                if uncertain:
                    unknown("nvidia-smi cannot verify GPU process utilization")
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
                unknown(exc)

    snapshot["ram_total_bytes"] = int(total)
    snapshot["ram_available_bytes"] = int(available)
    if total <= 0 or available < 0 or available > total:
        unknown("invalid RAM counters")
    else:
        snapshot["ram_used_percent"] = 100 * (total - available) / total
        if snapshot["ram_used_percent"] >= RAM_USED_PERCENT:
            busy("RAM usage >= {:.0f}%".format(RAM_USED_PERCENT))
    if snapshot["cpu_percent"] is not None and snapshot["cpu_percent"] >= CPU_BUSY_PERCENT:
        busy("system CPU usage >= {:.0f}%".format(CPU_BUSY_PERCENT))
    if snapshot["gpu_processes"]:
        busy("other 3D/compute GPU processes >= {:.0f}%".format(GPU_BUSY_PERCENT))
    return snapshot
