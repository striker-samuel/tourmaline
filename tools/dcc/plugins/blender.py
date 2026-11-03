"""Blender export operation: launch Blender headless with Python script."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Mapping

from dcc_runner import ExportPlugin, ProcessJob, validate_environment


class Plugin(ExportPlugin):
    name = "blender"
    software = "blender"
    extensions = (".blend",)
    priority = 50

    def build_job(self, source: Path, staged: Path, tools_dir: Path, *,
                  executable: str | None = None,
                  environment: Mapping[str, str] | None = None) -> ProcessJob:
        return ProcessJob(
            name="blender-export-gltf", software=self.software,
            executable=executable or os.environ.get("TOURMALINE_BLENDER", "blender"),
            executable_args=("--background", str(source)),
            script_switch="--python", script=tools_dir / "blender_export_gltf.py",
            script_args=("--", str(staged)),
            environment=validate_environment(environment or {}),
            working_dir=source.parent, priority=self.priority, tags=self.tags)
