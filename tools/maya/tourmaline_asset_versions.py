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
from urllib.parse import urlencode, urlsplit
from urllib.request import urlopen

from tourmaline_asset_cache import AssetClient

DEFAULT_SERVER = "http://127.0.0.1:8765"
WINDOW = "tourmalineAssetVersionsWindow"
ATTRS = {
    "asset_path": "tourmalineAssetPath",
    "source_id": "tourmalineSourceAssetId",
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
    """Local archive by default, or a configured authenticated NAS endpoint."""
    value = os.environ.get("TOURMALINE_ASSET_URL", DEFAULT_SERVER).rstrip("/")
    parts = urlsplit(value)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        raise ValueError("invalid TOURMALINE_ASSET_URL")
    return value


def _client():
    """None in legacy localhost mode; NAS mode requires a bearer token."""
    url = _server()
    token = os.environ.get("TOURMALINE_ASSET_TOKEN", "")
    host = urlsplit(url).hostname
    if not token and host not in ("127.0.0.1", "localhost"):
        raise ValueError("set TOURMALINE_ASSET_TOKEN for the shared repository")
    if not token:
        return None
    return AssetClient(url, token, cache=os.environ.get("TOURMALINE_ASSET_CACHE_DIR"))


def _api(endpoint, **params):
    client = _client()
    if client is not None:
        return client.get(endpoint, **{k: v for k, v in params.items() if v is not None})
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
    """Source identities are separate from the cooked build catalog."""
    client = _client()
    if client is None:
        assets = _api("/v1/assets")["assets"]
    else:
        assets = [
            {"path": row["current_path"], "source_id": row["asset_id"],
             "missing": row["missing"], "revision": row["revision"]}
            for row in client.sources()]
    needle = search.casefold().strip()
    return [a for a in assets if needle in a["path"].casefold()]


def versions(path, source_id=None):
    client = _client()
    if client is not None and source_id:
        return client.revisions(source_id)
    return _api("/v1/versions", path=path)["versions"]


def resolve(path, revision=None, pin=True, source_id=None):
    """Resolve immutable bytes into this workstation's cache, not on the NAS."""
    client = _client()
    if client is not None:
        if source_id:
            return client.resolve_source(source_id, revision, pin=pin)
        return client.resolve_path(path, revision, pin=pin)
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
        "source_id": item.get("asset_id", ""),
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


def import_version(path, revision, reference=False, source_id=None):
    """Bring an archived OBJ/FBX/Maya source into the current Maya scene.

    References are supported for .ma/.mb; imports for .obj/.fbx/.ma/.mb.
    Other types (e.g. textures) can still be resolved for texture nodes.
    """
    cmds = _maya()
    item = resolve(path, revision, pin=True, source_id=source_id)
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
            row[label] = (cmds.getAttr(node + "." + attr) or "") if cmds.attributeQuery(
                attr, node=node, exists=True) else ""
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
    source_id = (cmds.getAttr(record_node + "." + ATTRS["source_id"]) or "") if (
        cmds.attributeQuery(ATTRS["source_id"], node=record_node, exists=True)) else ""
    item = resolve(path, revision, pin=True, **({"source_id": source_id} if source_id else {}))
    if Path(item["local_path"]).suffix.lower() not in (".ma", ".mb"):
        raise ValueError("Cannot reference this format")
    cmds.file(item["local_path"], loadReference=ref_node)
    for field, value in (("revision", str(item["revision"])),
                         ("digest", item["sha256"]),
                         ("cache_path", item["local_path"])):
        cmds.setAttr(record_node + "." + ATTRS[field], value, type="string")
    return item


def rehydrate_scene_references():
    """Reconnect saved Maya reference pins to this workstation's local cache.

    Useful after opening a scene authored on another machine: the previous
    user's cache path is not portable, but asset path + revision are. This
    deliberately leaves ordinary imported geometry unchanged.
    """
    resolved, errors = [], []
    for record in version_records():
        if record["mode"] != "reference":
            continue
        try:
            item = switch_reference_version(record["node"], int(record["revision"]))
            resolved.append({"node": record["node"], "local_path": item["local_path"]})
        except (ValueError, OSError, RuntimeError) as exc:
            errors.append({"node": record["node"], "error": str(exc)})
    return {"resolved": resolved, "errors": errors}


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
        asset = selected_asset()
        if not asset:
            return
        try:
            rows = versions(asset["path"], asset.get("source_id"))
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
            available[label] = asset
            cmds.textScrollList(asset_list, edit=True, append=label)

    def do_rehydrate(*_):
        result = rehydrate_scene_references()
        cmds.inViewMessage(amg="Tourmaline: reloaded %s pinned references; %s errors" %
                           (len(result["resolved"]), len(result["errors"])),
                           pos="midCenterTop", fade=True)
        for error in result["errors"]:
            cmds.warning("%s: %s" % (error["node"], error["error"]))

    def do_import(*_):
        asset, rev = selected_asset(), selected_revision()
        if asset is None or rev is None:
            cmds.warning("Select a source asset and a revision")
            return
        try:
            node = import_version(asset["path"], rev,
                                  reference=cmds.checkBox(reference, query=True, value=True),
                                  source_id=asset.get("source_id"))
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
            data = resolve(asset["path"], rev, pin=True,
                           source_id=asset.get("source_id"))
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
    cmds.button(label="Repair this scene's pinned Maya references",
                command=do_rehydrate)
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
