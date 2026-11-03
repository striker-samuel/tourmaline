"""Maya export operation: invoke mayapy with a studio-replaceable script."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Mapping

from dcc_runner import ExportPlugin, ProcessJob, validate_environment


class Plugin(ExportPlugin):
    name = "maya"
    software = "maya"
    extensions = (".ma", ".mb")
    priority = 50

    def build_job(self, source: Path, staged: Path, tools_dir: Path, *,
                  executable: str | None = None,
                  environment: Mapping[str, str] | None = None) -> ProcessJob:
        # mayapy SCRIPT SOURCE OUTPUT; scripts can import Maya libraries.
        env = {"MAYA_DISABLE_CIP": "1", "MAYA_DISABLE_CER": "1"}
        env.update(validate_environment(environment or {}))
        return ProcessJob(
            name="maya-export-gltf", software=self.software,
            executable=executable or os.environ.get("TOURMALINE_MAYAPY", "mayapy"),
            script=tools_dir / "maya_export_gltf.py",
            script_args=(str(source), str(staged)),
            environment=env, working_dir=source.parent,
            priority=self.priority, tags=self.tags)
