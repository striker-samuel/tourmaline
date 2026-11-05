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
# WYSIWYG scene composition uses ImGui's desktop-space rectangle, but
# Vulkan's swapchain offsets must be local to the primary native window.
render = (src / "editor_render.cpp").read_text(encoding="utf-8")
compose = render.split("void EditorApp::RecordLiveViewComposePass(", 1)[1]
assert "const ImVec2 origin = mainViewport->Pos;" in compose
for edge, axis in (("x0", "x"), ("x1", "x"), ("y0", "y"), ("y1", "y")):
    assert re.search(
        rf"const float {edge} = std::clamp\(m_wysiwygLiveRect(?:Min|Max)\.{axis} - origin\.{axis},",
        compose,
    ), f"{edge} must be converted to main-window-relative coordinates"

# Inspector layout regression: never clip names from the LEFT, and
# keep the add-component picker after node flags/material sections.
attrs = (src / "editor_attr_ui.h").read_text(encoding="utf-8")
assert "const bool stacked = available < colW + 112.0f;" in attrs
assert "ImGui::TextWrapped(\"%s\", shown)" in attrs
assert "ImGui::PushClipRect(" not in attrs.split("inline void LabelCell(", 1)[1].split("// Component header", 1)[0]
assert sections.index('SeparatorText("Node flags")') < sections.index('SeparatorText("Material component")')
assert sections.index('SeparatorText("Material component")') < sections.index('Button("Add component...")')
material_ui = (src / "ui" / "panels" / "material_panel.cpp").read_text(encoding="utf-8")
assert material_ui.index('attr::Color("Base color"') < material_ui.index('SeparatorText("Texture slots")')
assert 'SeparatorText("Material flags")' in material_ui
assert 'attr::LabelCell(s.label, nullptr)' in material_ui
assert 'Remove material component' in sections
assert 'None (editor default material)' in sections

# Live gizmos use the renderer debug primitive source; legacy previews
# retain their own pass for non-live viewport modes.
gizmo = (src / "editor_gizmo.cpp").read_text(encoding="utf-8")
assert 'debugOutput->Line(' in gizmo
assert 'SetDebugDrawSource([this]' in render

print(f"Editor UI structural checks passed ({len(definitions)} EditorApp definitions).")
