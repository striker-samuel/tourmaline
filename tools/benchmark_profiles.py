"""Synthetic benchmark resource ceilings, not hardware emulation."""

PROFILES = {
    "native": (None, None),
    "ps4": (7, 4096),
    "ps5": (14, 12288),
    "rtx4060": (None, 8192),
}


def resolve_profile(name, cpu_limit=None, vram_mb=None):
    cpu, vram = PROFILES[name]
    cpu = cpu if cpu_limit is None else cpu_limit
    vram = vram if vram_mb is None else vram_mb
    if cpu is not None and not 1 <= cpu <= 1024:
        raise ValueError("CPU limit must be 1..1024 logical CPUs")
    if vram is not None and not 1 <= vram <= 1048576:
        raise ValueError("VRAM limit must be 1..1048576 MiB")
    return {"name": name, "cpu_limit": cpu, "vram_mb": vram}


def profile_applied(profile, log):
    """Reject successful-looking output from an older, uncapped executable."""
    for key, prefix in (("cpu_limit", "[Benchmark] logical CPU limit="),
                        ("vram_mb", "[Benchmark] VMA device-local limit=")):
        value = profile[key]
        if value is not None:
            expected = prefix + str(value) + (" MiB" if key == "vram_mb" else "")
            if expected not in log.splitlines():
                return False
    return True
