"""Local Deadline-style process executor shared by DCC export plugins.

A plugin describes a job: executable, arguments, script, script arguments,
environment, working directory and timeout. The executor owns process
lifecycle and reports failures. A farm adapter can consume the same job
description later without importing Maya/Blender on the editor machine.
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
import os
import re
import subprocess
import sys
from typing import Mapping

_ENV_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


@dataclass(frozen=True)
class ProcessJob:
    name: str
    software: str
    executable: str
    script: Path
    script_args: tuple[str, ...] = ()
    executable_args: tuple[str, ...] = ()
    script_switch: str = ""  # e.g. Blender's --python (mayapy needs none)
    environment: Mapping[str, str] = field(default_factory=dict)
    working_dir: Path | None = None
    timeout_seconds: float = 1800
    priority: int = 50
    tags: tuple[str, ...] = ("dcc", "export", "gltf")

    def command(self) -> list[str]:
        if not self.executable or not self.script.is_file():
            raise ValueError("DCC executable or export script is missing")
        return [self.executable, *self.executable_args,
                *([self.script_switch] if self.script_switch else []),
                str(self.script), *self.script_args]


class ExportPlugin(ABC):
    """Trust-boundary: these Python plugin files are installed with Editor.

    Projects only select a known plugin name; no project-supplied script is
    imported by the editor runner.
    """
    name: str = ""
    software: str = ""
    extensions: tuple[str, ...] = ()
    priority: int = 50
    tags: tuple[str, ...] = ("dcc", "export", "gltf")

    def accepts(self, source: Path) -> bool:
        return source.suffix.lower() in self.extensions

    @abstractmethod
    def build_job(self, source: Path, staged: Path, tools_dir: Path, *,
                  executable: str | None = None,
                  environment: Mapping[str, str] | None = None) -> ProcessJob:
        """Describe the external export process without starting it."""


def validate_environment(overrides: Mapping[str, str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for key, value in overrides.items():
        if not isinstance(key, str) or not _ENV_NAME.fullmatch(key):
            raise ValueError("invalid environment variable name: " + repr(key))
        if not isinstance(value, str) or "\x00" in value:
            raise ValueError("invalid environment value for " + key)
        result[key] = value
    return result


def run_local(job: ProcessJob) -> None:
    """Run an executable directly (never through a shell), propagating failure.

    This is intentionally blocking for use on Editor's existing worker pool.
    The plugin describes the job; an executor owns actual process launch.
    """
    environment = os.environ.copy()
    environment.update(validate_environment(job.environment))
    if job.timeout_seconds <= 0:
        raise ValueError("DCC job timeout must be positive")
    argv = job.command()
    print(f"[DCC] {job.name} ({job.software}, priority {job.priority})",
          flush=True)
    try:
        result = subprocess.run(
            argv, env=environment,
            cwd=str(job.working_dir) if job.working_dir else None,
            timeout=job.timeout_seconds,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{job.name} timed out after {job.timeout_seconds:g}s; "
            "old publication intact") from exc
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n",
              flush=True)
    if result.returncode:
        # Keep a short diagnostic in the exception forwarded to the caller.
        tail = (result.stdout or "")[-1500:].strip()
        raise RuntimeError(
            f"{job.name} exited {result.returncode}; old publication intact"
            + (": " + tail if tail else ""))
