"""Headless Maya exporter invoked by mayapy for the Tourmaline DCC runner.

Requires the open-source maya-glTF Python exporter to be installed on Maya's
PYTHONPATH (it exposes glTFExport.export). Maya does NOT include a standard
glTF exporter. Replace this adapter if the studio uses a different plugin.
"""
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        raise ValueError("usage: mayapy maya_export_gltf.py SOURCE.ma OUTPUT.gltf")
    source, output = map(lambda p: Path(p).resolve(), sys.argv[1:])
    import maya.standalone
    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds
        cmds.file(str(source), open=True, force=True)
        try:
            import glTFExport
        except ImportError as exc:
            raise RuntimeError(
                "Install/configure the maya-glTF exporter in mayapy; "
                "it must provide glTFExport.export()") from exc
        # Embedded buffers/images: one immutable publication file, even
        # if the authored Maya scene depends on multiple textures.
        glTFExport.export(str(output), resource_format="embedded", anim="keyed")
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
