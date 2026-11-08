#!/usr/bin/env python3
"""Validate sample 04's committed glTF geometry and bell WAV without engine dependencies."""
import json
import math
from pathlib import Path
import struct
import wave

ROOT = Path(__file__).resolve().parent / "project/content"
model = ROOT / "models/game/game.gltf"
doc = json.loads(model.read_text(encoding="utf-8"))
data = (model.parent / doc["buffers"][0]["uri"]).read_bytes()
assert len(data) == doc["buffers"][0]["byteLength"]
primitive = doc["meshes"][0]["primitives"][0]
assert {"POSITION", "NORMAL", "TEXCOORD_0"} <= primitive["attributes"].keys()

def read_accessor(index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[acc["type"]]
    fmt = {5126: "f", 5123: "H"}[acc["componentType"]]
    stride = struct.calcsize("<" + fmt * width)
    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    end = offset + acc["count"] * stride
    assert end <= view.get("byteOffset", 0) + view["byteLength"]
    return list(struct.iter_unpack("<" + fmt * width, data[offset:end]))

positions = read_accessor(primitive["attributes"]["POSITION"])
normals = read_accessor(primitive["attributes"]["NORMAL"])
uvs = read_accessor(primitive["attributes"]["TEXCOORD_0"])
indices = [x[0] for x in read_accessor(primitive["indices"])]
assert len(positions) == 49 and len(normals) == 49 and len(uvs) == 49
assert len(indices) == 216 and all(0 <= i < len(positions) for i in indices)
assert all(math.isfinite(v) for p in positions + normals + uvs for v in p)
assert all(p[1] == 0 for p in positions)
assert all(n == (0, 1, 0) for n in normals)
assert all(0 <= u <= 1 and 0 <= v <= 1 for u, v in uvs)
for i in range(0, len(indices), 3):
    a, b, c = (positions[j] for j in indices[i:i+3])
    ab = (b[0]-a[0], b[2]-a[2])
    ac = (c[0]-a[0], c[2]-a[2])
    assert ab[1]*ac[0] - ab[0]*ac[1] > 0, "triangle must face +Y"

with wave.open(str(ROOT / "audio/bells.wav"), "rb") as f:
    assert f.getnchannels() == 1 and f.getsampwidth() == 2
    assert f.getframerate() == 48000 and f.getnframes() >= 48000 * 3
    pcm = struct.unpack("<" + "h" * f.getnframes(), f.readframes(f.getnframes()))
    assert max(abs(x) for x in pcm) < 32767, "bell clip is saturated"
    assert abs(pcm[0]) < 32 and abs(pcm[-1]) < 32, "loop boundary must be silent"

scene = (ROOT / "scenes/game.tour").read_text(encoding="utf-8")
assert 'playOnLoad -type "bool" -v off' in scene, "bell scene source must not autoplay"
print("Sample 04 assets valid")
