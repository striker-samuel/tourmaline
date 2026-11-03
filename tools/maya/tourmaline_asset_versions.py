"""Maya client for Tourmaline's offline source-version archive.

Install by adding this directory to Maya's Python sys.path, then:
    import tourmaline_asset_versions as versions
    versions.show_picker()

The service only exposes localhost JSON; Maya receives a verified, per-user
cache path rather than repeatedly reading large files from the SMB share.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import urlopen

DEFAULT_SERVER = "http://127.0.0.1:8765"
WINDOW = "tourmalineAssetVersionsWindow"
ATTRS = {
    "asset_path": "tourmalineAssetPath",
    "revision": "tourmalineRevision",
    "digest": "tourmalineSha256",
    "cache_path": "tourmalineCachePath",
    "mode": "tourmalineImportMode",
    "reference_node": "tourmalineReferenceNode",
}


def _maya():
    try:
        import maya.cmds as cmds
        return cmds
    except ImportError as exc:
        raise RuntimeError("Open this tool from Autodesk Maya's Python environment") from exc


def _server():
    # Restrict to localhost: this tool is intentionally offline and local.
    value = os.environ.get("TOURMALINE_ASSET_URL", DEFAULT_SERVER).rstrip("/")
    if value not in ("http://127.0.0.1:8765", "http://localhost:8765"):
        # Multiple localhost ports can be supported without allowing arbitrary servers.
        match = re.fullmatch(r"http://(127\.0\.0\.1|localhost):([0-9]{2,5})", value)
        if not match or not (1024 <= int(match.group(2)) <= 65535):
            raise ValueError("TOURMALINE_ASSET_URL must be http://127.0.0.1:<port>")
    return value


def _api(endpoint, **params):
    url = _server() + endpoint
    if params:
        url += "?" + urlencode({key: value for key, value in params.items()
                                if value is not None})
    try:
        with urlopen(url, timeout=10) as response:
            return json.load(response)
    except HTTPError as exc:
        raise RuntimeError("Asset service returned HTTP %s for %s" % (exc.code, endpoint)) from exc
    except (OSError, URLError, ValueError) as exc:
        raise RuntimeError("Cannot reach the localhost asset service: %s" % exc) from exc


def available_assets(search=""):
    """List archived paths. A missing file on G: is still available in history."""
    assets = _api("/v1/assets")["assets"]
    needle = search.casefold().strip()
    return [a for a in assets if needle in a["path"].casefold()]


def versions(path):
    return _api("/v1/versions", path=path)["versions"]


def resolve(path, revision=None, pin=True):
    """Resolve to a local file. Never mutates the shared project folder."""
    item = _api("/v1/resolve", path=path, revision=revision, pin=int(bool(pin)))
    local = Path(item["local_path"])
    if not local.is_file():
        raise FileNotFoundError("Asset service returned an unavailable cache path: %s" % local)
    return item


def _safe_namespace(path):
    name = re.sub(r"[^A-Za-z0-9_]", "_", Path(path).stem)
    if not name or name[0].isdigit():
        name = "asset_" + name
    return "tm_" + name


def _record(cmds, item, source_path, mode, reference_node=""):
    """Save version selection inside the Maya scene, not beside shared sources."""
    node = cmds.createNode("network", name="tourmalineAssetVersion#")
    data = {
        "asset_path": source_path,
        "revision": str(item["revision"]),
        "digest": item["sha256"],
        "cache_path": item["local_path"],
        "mode": mode,
        "reference_node": reference_node,
    }
    for label, value in data.items():
        name = ATTRS[label]
        cmds.addAttr(node, longName=name, dataType="string")
        cmds.setAttr(node + "." + name, value, type="string")
    return node


def import_version(path, revision, reference=False):
    """Bring an archived OBJ/FBX/Maya source into the current Maya scene.

    References are supported for .ma/.mb; imports for .obj/.fbx/.ma/.mb.
    Other types (e.g. textures) can still be resolved for texture nodes.
    """
    cmds = _maya()
    item = resolve(path, revision, pin=True)
    local = item["local_path"]
    suffix = Path(local).suffix.lower()
    if suffix not in (".obj", ".fbx", ".ma", ".mb"):
        raise ValueError("This format is not a Maya scene/geometry file. Use resolve() for textures.")
    if reference and suffix not in (".ma", ".mb"):
        raise ValueError("Use Import for OBJ/FBX. Reference is supported for .ma/.mb.")
    if suffix == ".fbx":
        cmds.loadPlugin("fbxmaya", quiet=True)
    if suffix == ".obj":
        cmds.loadPlugin("objExport", quiet=True)

    namespace = _safe_namespace(path)
    reference_node = ""
    if reference:
        cmds.file(local, reference=True, namespace=namespace,
                  ignoreVersion=True, options="v=0;")
        reference_node = cmds.referenceQuery(local, referenceNode=True)
        mode = "reference"
    else:
        kwargs = dict(i=True, namespace=namespace, ignoreVersion=True,
                      mergeNamespacesOnClash=False)
        if suffix == ".obj":
            kwargs["type"] = "OBJ"
        elif suffix == ".fbx":
            kwargs["type"] = "FBX"
        cmds.file(local, **kwargs)
        mode = "import"

    return _record(cmds, item, path, mode, reference_node=reference_node)


def version_records():
    """Read the working-version selections stored in this Maya scene."""
    cmds = _maya()
    records = []
    for node in cmds.ls(type="network") or []:
        if not cmds.attributeQuery(ATTRS["asset_path"], node=node, exists=True):
            continue
        row = {"node": node}
        for label, attr in ATTRS.items():
            row[label] = cmds.getAttr(node + "." + attr) or ""
        records.append(row)
    return records


def switch_reference_version(record_node, revision):
    """Switch one Maya scene reference to another pinned local revision."""
    cmds = _maya()
    if not cmds.attributeQuery(ATTRS["asset_path"], node=record_node, exists=True):
        raise ValueError("Not a Tourmaline version record")
    path = cmds.getAttr(record_node + "." + ATTRS["asset_path"])
    mode = cmds.getAttr(record_node + "." + ATTRS["mode"])
    ref_node = cmds.getAttr(record_node + "." + ATTRS["reference_node"])
    if mode != "reference" or not ref_node or not cmds.objExists(ref_node):
        raise ValueError("Only Maya file references can be switched in place")
    item = resolve(path, revision, pin=True)
    if Path(item["local_path"]).suffix.lower() not in (".ma", ".mb"):
        raise ValueError("Cannot reference this format")
    cmds.file(item["local_path"], loadReference=ref_node)
    for field, value in (("revision", str(item["revision"])),
                         ("digest", item["sha256"]),
                         ("cache_path", item["local_path"])):
        cmds.setAttr(record_node + "." + ATTRS[field], value, type="string")
    return item


def show_picker():
    """Native Maya GUI: choose a source path, choose an immutable revision."""
    cmds = _maya()
    if cmds.window(WINDOW, exists=True):
        cmds.deleteUI(WINDOW)
    window = cmds.window(WINDOW, title="Tourmaline — Source Asset Versions",
                         widthHeight=(650, 540), sizeable=True)
    cmds.columnLayout(adjustableColumn=True, rowSpacing=7)
    cmds.text(label="Source files can be overwritten on the share; choose preserved revisions here.",
              align="left")
    query = cmds.textFieldGrp(label="Filter", text="")
    asset_list = cmds.textScrollList(numberOfRows=13, allowMultiSelection=False)
    cmds.text(label="Archived versions (most recent first)", align="left")
    rev_list = cmds.textScrollList(numberOfRows=8, allowMultiSelection=False)
    reference = cmds.checkBox(label="Reference (Maya .ma/.mb only)", value=False)

    available = {}
    version_map = {}

    def selected_asset():
        labels = cmds.textScrollList(asset_list, query=True, selectItem=True) or []
        return available.get(labels[0]) if labels else None

    def selected_revision():
        labels = cmds.textScrollList(rev_list, query=True, selectItem=True) or []
        return version_map.get(labels[0]) if labels else None

    def refresh_versions(*_):
        cmds.textScrollList(rev_list, edit=True, removeAll=True)
        version_map.clear()
        path = selected_asset()
        if not path:
            return
        try:
            rows = versions(path)
        except Exception as exc:
            cmds.warning(str(exc))
            return
        for v in rows:
            label = "v%s  |  %s  |  %s bytes" % (
                v["revision"], v["captured_at"], v["size"])
            version_map[label] = v["revision"]
            cmds.textScrollList(rev_list, edit=True, append=label)

    def refresh_assets(*_):
        cmds.textScrollList(asset_list, edit=True, removeAll=True)
        cmds.textScrollList(rev_list, edit=True, removeAll=True)
        available.clear()
        try:
            rows = available_assets(cmds.textFieldGrp(query, query=True, text=True))
        except Exception as exc:
            cmds.warning(str(exc))
            return
        for asset in rows:
            path = asset["path"]
            label = path + ("  [REMOVED FROM SHARE]" if asset["missing"] else "")
            available[label] = path
            cmds.textScrollList(asset_list, edit=True, append=label)

    def do_import(*_):
        path, rev = selected_asset(), selected_revision()
        if path is None or rev is None:
            cmds.warning("Select a source asset and a revision")
            return
        try:
            node = import_version(path, rev,
                                  reference=cmds.checkBox(reference, query=True, value=True))
        except Exception as exc:
            cmds.warning("Import failed: %s" % exc)
            return
        cmds.inViewMessage(amg="Tourmaline imported revision %s; metadata: %s" % (rev, node),
                           pos="midCenterTop", fade=True)

    def do_resolve(*_):
        path, rev = selected_asset(), selected_revision()
        if path is None or rev is None:
            cmds.warning("Select a source asset and a revision")
            return
        try:
            data = resolve(path, rev, pin=True)
            cmds.promptDialog(title="Pinned local file", message="Use this path for texture nodes:",
                              text=data["local_path"], button=["OK"], defaultButton="OK")
        except Exception as exc:
            cmds.warning(str(exc))

    cmds.textFieldGrp(query, edit=True, changeCommand=refresh_assets)
    cmds.textScrollList(asset_list, edit=True, selectCommand=refresh_versions)
    cmds.rowLayout(numberOfColumns=3, adjustableColumn=1, columnWidth3=(200, 200, 200))
    cmds.button(label="Refresh assets", command=refresh_assets)
    cmds.button(label="Resolve texture / file", command=do_resolve)
    cmds.button(label="Import / Reference selected", command=do_import)
    cmds.setParent("..")
    cmds.showWindow(window)
    refresh_assets()
    return window


def install_shelf_button():
    """Run inside Maya once to add a permanent Asset Versions shelf button."""
    cmds = _maya()
    import maya.mel as mel
    top = mel.eval("$tmp=$gShelfTopLevel")
    shelf = "Tourmaline"
    if not cmds.shelfLayout(shelf, exists=True):
        cmds.shelfLayout(shelf, parent=top)
    # Maya's shelf command imports by name; make this module discoverable in
    # PYTHONPATH/MAYA_SCRIPT_PATH or place it in the user's scripts folder.
    cmds.shelfButton(parent=shelf, label="Asset Versions", annotation="Tourmaline version picker",
                     image1="pythonFamily.png", sourceType="python",
                     command="import tourmaline_asset_versions as tav; tav.show_picker()")
    return shelf
