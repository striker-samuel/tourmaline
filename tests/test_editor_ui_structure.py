#!/usr/bin/env python3
"""Dependency-free structure checks for the extracted editor UI.

Run: python3 tests/test_editor_ui_structure.py
Compilation and interactive Vulkan testing must be performed separately.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
src = root / "editor" / "src"
sources = list(src.rglob("*.cpp"))
assert sources, "Editor sources not found"

# Count method *definitions*, not calls. Methods have a qualified name
# followed by a parameter list and an opening brace (possibly on next line).
pattern = re.compile(r"\bEditorApp::(\w+)\s*\([^;{}]*?\)\s*(?:const\s*)?\{", re.S)
definitions = {}
for file in sources:
    code = file.read_text(encoding="utf-8")
    for match in pattern.finditer(code):
        definitions.setdefault(match.group(1), []).append(file.relative_to(root))
for name, owners in definitions.items():
    assert len(owners) == 1, f"Duplicate {name}: {owners}"

main = (src / "editor_ui.cpp").read_text(encoding="utf-8")
assert "panels.DrawAll(uiContext)" in main
for panel in ("Outliner", "Reference", "Assets", "Console",
              "Overrides", "Inspector", "Render Settings", "Material"):
    assert f'panels.Register("{panel}"' in main, panel

sections = (src / "ui" / "panels" / "inspector_panel.cpp").read_text(encoding="utf-8")
assert "sections.DrawAll(node)" in sections
for section in ("gi_volume", "physics", "trigger", "audio_room",
                "export_settings", "cubemap", "camera", "audio"):
    assert (src / "ui" / "inspector" / f"{section}_section.cpp").exists(), section

assert (src / "ui" / "panels" / "asset_inspection.cpp").exists()
assert (src / "ui" / "panels" / "asset_dcc.cpp").exists()
assert "GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS" in (
    root / "editor" / "CMakeLists.txt").read_text(encoding="utf-8")
print(f"Editor UI structural checks passed ({len(definitions)} EditorApp definitions).")
