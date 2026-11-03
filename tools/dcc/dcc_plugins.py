"""Discover trusted Python DCC operation plugins by filename.

Place a new <name>.py in tools/dcc/plugins, exporting a Plugin class
derived from ExportPlugin. There is no core dispatch switch to edit.
These modules are trusted executable pipeline code, not project assets.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import re

from dcc_runner import ExportPlugin

_VALID_PLUGIN = re.compile(r"[a-z][a-z0-9_]*\Z")


def plugin_directory() -> Path:
    return Path(__file__).resolve().parent / "plugins"


def available_plugins(directory: Path | None = None) -> list[str]:
    root = directory or plugin_directory()
    return sorted(p.stem for p in root.glob("*.py")
                  if p.name != "__init__.py" and _VALID_PLUGIN.fullmatch(p.stem))


def load_plugin(name: str, directory: Path | None = None) -> ExportPlugin:
    if not _VALID_PLUGIN.fullmatch(name):
        raise ValueError("invalid DCC plugin name")
    root = directory or plugin_directory()
    file = root / (name + ".py")
    if not file.is_file():
        raise ValueError(
            f"no DCC plugin {name!r} (available: "
            + ", ".join(available_plugins(root)) + ")")
    module_spec = importlib.util.spec_from_file_location(
        f"_tourmaline_dcc_plugin_{name}", file)
    if module_spec is None or module_spec.loader is None:
        raise ValueError("cannot load DCC plugin: " + name)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    cls = getattr(module, "Plugin", None)
    if not isinstance(cls, type) or not issubclass(cls, ExportPlugin):
        raise ValueError("DCC plugin must expose Plugin(ExportPlugin)")
    instance = cls()
    if instance.name != name or not instance.extensions:
        raise ValueError("DCC plugin metadata disagrees with its filename")
    return instance
