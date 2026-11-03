"""Headless Blender adapter for the Tourmaline DCC glTF build stage."""
import sys
from pathlib import Path

import bpy


def main():
    if "--" not in sys.argv:
        raise ValueError("missing published glTF output argument")
    args = sys.argv[sys.argv.index("--") + 1:]
    if len(args) != 1:
        raise ValueError("expected exactly one output path")
    output = str(Path(args[0]).resolve())
    result = bpy.ops.export_scene.gltf(
        filepath=output, export_format="GLTF_EMBEDDED")
    if "FINISHED" not in result:
        raise RuntimeError("Blender glTF exporter did not finish")


if __name__ == "__main__":
    main()
