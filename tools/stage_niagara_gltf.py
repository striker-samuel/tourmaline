#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# stage_niagara_gltf.py — stage the known-good Niagara bistro.gltf for a
# direct (no-pak) geometry A/B against the v5.2 Blender conversion.
#
# IMPORTANT: the authoring shims are KEPT VERBATIM. RootNode's 100x scale
# is the FBX cm->m unit conversion and Level's -90d X rotation is the
# DCC Z-up -> glTF Y-up conversion. (World AABB with shims intact:
# 111 x 32 x 119 m; with them neutralized: 1.1 x 1.2 x 0.3 m — the old
# cook_bistro.py neutralization predates the node-transform conjugation
# fix in engine/src/Utils/gltf.cpp and breaks the scene: tiny + on its
# side.) Textures stay unresolved (DDS/PNG upstream): materials render
# with baseColorFactor, fine for a geometry correctness check.
import shutil
import sys
from pathlib import Path

SRC = Path(r"F:/REPOS/niagara_bistro")
DST = Path(r"F:/content/models/BistroNiagara")

def main():
    DST.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SRC / "bistro.gltf", DST / "bistro.gltf")
    for blob in ("bistro.bin", "bistro-anim.bin"):
        shutil.copy2(SRC / blob, DST / blob)
        print(f"[niagara] copied {blob}")
    print(f"[niagara] staged {DST} (shims intact)")

if __name__ == "__main__":
    sys.exit(main())
