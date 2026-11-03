#!/usr/bin/env python3
"""Fast pre-build source/art sanity checks. Not a replacement for a GPU smoke test."""
import base64
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gltf_path = root / "project/content/models/placeholder/cube.gltf"
scene = (root / "project/content/scenes/main.tour").read_text()
recipe = (root / "project/game.tourrecipe").read_text()
source = (root / "src/main.cpp").read_text()
cmake = (root / "CMakeLists.txt").read_text()
data = json.loads(gltf_path.read_text())
assert data["asset"]["version"] == "2.0"
assert len(data["nodes"]) >= 3 and len(data["meshes"]) >= 2
assert any(node.get("extensions", {}).get("KHR_lights_punctual") for node in data["nodes"])
assert any(m.get("pbrMetallicRoughness", {}).get("baseColorTexture") for m in data["materials"])
buf = data["buffers"][0]
binary = base64.b64decode(buf["uri"].split(",", 1)[1])
assert len(binary) == buf["byteLength"]
for view in data["bufferViews"]:
    assert view["byteOffset"] + view["byteLength"] <= len(binary)
for image in data["images"]:
    if image["uri"].startswith("data:"):
        assert base64.b64decode(image["uri"].split(",", 1)[1]).startswith(b"\x89PNG\r\n\x1a\n")
assert "cube.geometry" in scene
assert "bake gi off" in recipe
assert "content/models/placeholder/**" in recipe
assert "app::RunGame" in source and "SetExtraGameUi" in source
assert "tourmaline_apphost" in cmake
for file in (root / "src").rglob("*"):
    if not file.is_file():
        continue
    txt = file.read_text()
    for banned in ("SDL_CreateRenderer", "SDL_RenderTexture", "vkCreate", "vkCmd", "#include <vulkan/"):
        assert banned not in txt, (file, banned)
print("Native Tourmaline starter: static assets and source checks passed")
