#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Offline image/geometry asset reverse search, explainable scoring, and image fab CLI.

Usage: python tools/asset_reverse_search.py {index,search,describe,fab} --help
Requires Pillow for image operations. Only fab --execute sends a network request.
"""
from __future__ import annotations

import argparse
import base64
from contextlib import ExitStack
import hashlib
import io
import json
import math
import os
from pathlib import Path
import sys
import urllib.error
import urllib.request

try:
    from PIL import Image
except ImportError:
    Image = None

import geometry_capture_atlas as capture

SCHEMA = "tourmaline.asset-reverse-search.v1"
DIMENSIONS = ("shape", "color", "normal", "roughness", "metallic", "occlusion", "emissive", "size")
DEFAULT_WEIGHTS = {
    "shape": 0.30, "color": 0.20, "normal": 0.10,
    "roughness": 0.10, "metallic": 0.05, "occlusion": 0.05,
    "emissive": 0.05, "size": 0.15,
}
CAPTURE_CHANNELS = ("coverage", "albedo", "normal", "orm", "emissive")
VIEWS = tuple(view[0] for view in capture.VIEWS)


class SearchError(ValueError):
    """Invalid search input or index, with an actionable message."""


def require_pillow():
    if Image is None:
        raise SearchError("Pillow required: python -m pip install pillow")


def clamp(v):
    return min(1.0, max(0.0, v))


def mean(values):
    return sum(values) / len(values) if values else 0.0


def sample_tiles(tiles, grid):
    """Turn material-channel images into compact, channel-aligned grid data."""
    require_pillow()
    result = {}
    mode = Image.Resampling if hasattr(Image, "Resampling") else Image
    for channel in CAPTURE_CHANNELS:
        if channel not in tiles:
            continue
        tile = tiles[channel].convert("RGBA").resize((grid, grid), mode.BOX)
        raw = tile.tobytes()
        if channel == "coverage":
            data = raw[3::4]
        else:
            data = b"".join(raw[i:i + 3] for i in range(0, len(raw), 4))
        result[channel] = base64.b64encode(data).decode("ascii")
    return result


def decoded_view(view, grid):
    pixels = grid * grid
    result = {}
    for name, value in view.items():
        if name not in CAPTURE_CHANNELS or not isinstance(value, str):
            raise SearchError("invalid search descriptor channel")
        try:
            raw = base64.b64decode(value, validate=True)
        except (ValueError, base64.binascii.Error) as exc:
            raise SearchError("malformed base64 search descriptor") from exc
        if len(raw) != pixels * (1 if name == "coverage" else 3):
            raise SearchError("search descriptor length/grid mismatch")
        result[name] = raw
    if "coverage" not in result or "albedo" not in result:
        raise SearchError("search descriptor missing coverage or albedo")
    return result


def load_search_index(path):
    path = Path(path).resolve()
    doc = json.loads(path.read_text(encoding="utf-8"))
    if doc.get("schema") != SCHEMA or not isinstance(doc.get("assets"), list):
        raise SearchError("not an asset reverse-search index; run 'index' first")
    grid = doc.get("grid")
    if not isinstance(grid, int) or not 2 <= grid <= 32:
        raise SearchError("unsupported descriptor grid size")
    if set(doc.get("views", [])) != set(VIEWS):
        raise SearchError("unexpected capture views in index")
    for asset in doc["assets"]:
        for view in asset.get("features", {}).values():
            decoded_view(view, grid)
    return doc


def atlas_to_search_index(atlas_index, output, mip=None, grid=8):
    """Read exact rects from a capture index; never derive rect offsets by shifting."""
    require_pillow()
    atlas_index = Path(atlas_index).resolve()
    doc = json.loads(atlas_index.read_text(encoding="utf-8"))
    pages = doc.get("mips", [])
    if doc.get("version") != 1 or not pages or not doc.get("assets"):
        raise SearchError("expected a nonempty geometry_capture_atlas index.json v1")
    if mip is None:
        page = min(pages, key=lambda p: abs(math.log2(max(1, p["face_size"]) / 32)))
    else:
        page = next((p for p in pages if p.get("level") == mip), None)
        if page is None:
            raise SearchError(f"mip {mip} is not in the capture atlas")
    level = page["level"]
    result = {"schema": SCHEMA, "grid": grid, "mip": level,
              "face_size": page["face_size"], "views": list(VIEWS),
              "atlas_index": os.path.relpath(atlas_index, Path(output).resolve().parent),
              "assets": []}
    with ExitStack() as opened:
        images = {}
        for channel in CAPTURE_CHANNELS:
            filename = page.get("files", {}).get(channel)
            if not filename or Path(filename).name != filename:
                raise SearchError(f"invalid {channel} atlas filename")
            image = opened.enter_context(Image.open(atlas_index.parent / filename))
            if image.size != (page["width"], page["height"]):
                raise SearchError(f"{filename}: bitmap dimensions differ from index.json")
            images[channel] = image
        for asset in doc["assets"]:
            placement = next((m for m in asset.get("mips", []) if m.get("level") == level), None)
            if placement is None:
                raise SearchError("asset has no placement for selected mip")
            features = {}
            for view in VIEWS:
                box = placement.get("views", {}).get(view)
                if not box:
                    raise SearchError(f"asset is missing {view} capture")
                x, y, w, h = (box[k] for k in ("x", "y", "width", "height"))
                if not all(isinstance(v, int) for v in (x, y, w, h)) or x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > page["width"] or y + h > page["height"]:
                    raise SearchError("asset view rect is outside atlas image")
                features[view] = sample_tiles({channel: image.crop((x, y, x + w, y + h))
                                               for channel, image in images.items()}, grid)
            result["assets"].append({
                "source": asset["source"], "geometry_guid": asset.get("geometry_guid", ""),
                "sha256": asset.get("sha256", ""), "bounds": asset["bounds"],
                "triangle_count": asset.get("triangle_count", 0),
                "materials": asset.get("materials", []), "features": features,
            })
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    Path(output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def resolve_geometry(path):
    """Accept serialized MESH v3 .geometry/.mesh or a .mesh geometry= path reference."""
    path = Path(path).resolve()
    if path.suffix.lower() not in (".geometry", ".mesh"):
        raise SearchError("geometry query must be a .geometry or .mesh file")
    with path.open("rb") as file:
        header = file.read(8)
    if header == b"HSEM\x03\x00\x00\x00":  # little-endian 0x4D455348 + v3
        return path
    if path.suffix.lower() == ".geometry":
        raise SearchError("unsupported .geometry payload; expected MESH v3")
    try:
        rows = capture.read_kv(path)
    except UnicodeError as exc:
        raise SearchError(".mesh must contain MESH v3 bytes or a geometry=<path> reference") from exc
    source = rows.get("geometry")
    if not source:
        raise SearchError(".mesh is not MESH v3 or an explicit geometry=<path> reference")
    resolved = (path.parent / source).resolve()
    if resolved == path or resolved.suffix.lower() != ".geometry":
        raise SearchError(".mesh geometry reference must name a .geometry file")
    return resolve_geometry(resolved)


def descriptor_for_geometry(path, doc):
    path = resolve_geometry(path)
    asset = capture.prepare(path, doc["face_size"])
    return {"source": path.as_posix(), "geometry_guid": asset["guid"],
            "bounds": {"min": asset["aabb_min"], "max": asset["aabb_max"],
                       "size": [b - a for a, b in zip(asset["aabb_min"], asset["aabb_max"])]},
            "features": {view: sample_tiles(layers, doc["grid"])
                         for view, layers in asset["captures"].items()}}


def descriptor_for_image(path, doc, view="+Z", size=None):
    require_pillow()
    if view not in VIEWS:
        raise SearchError(f"invalid view: {view}")
    with Image.open(path) as src:
        image = src.convert("RGBA")
    # A generated photograph with opaque background has no available silhouette.
    # Do not infer shape from RGB or pretend ORM/normal channels exist.
    if not image.getchannel("A").getbbox():
        raise SearchError("query image is fully transparent")
    desc = sample_tiles({"coverage": image, "albedo": image}, doc["grid"])
    return {"source": str(path), "geometry_guid": "", "bounds": {"size": size} if size else None,
            "features": {view: desc}}

def visual_scores(query, candidate, grid):
    intersections, unions = 0.0, 0.0
    sums = {key: 0.0 for key in ("color", "normal", "roughness", "metallic", "occlusion", "emissive")}
    weights = {key: 0.0 for key in sums}
    for name, encoded in query["features"].items():
        if name not in candidate["features"]:
            continue
        q = decoded_view(encoded, grid)
        c = decoded_view(candidate["features"][name], grid)
        for i, (a8, b8) in enumerate(zip(q["coverage"], c["coverage"])):
            a, b = a8 / 255, b8 / 255
            intersections += min(a, b)
            unions += max(a, b)
            overlap = min(a, b)
            if overlap == 0:
                continue
            for channel, kind, indices in (
                    ("albedo", "color", (0, 1, 2)),
                    ("orm", "occlusion", (0,)),
                    ("orm", "roughness", (1,)),
                    ("orm", "metallic", (2,)),
                    ("emissive", "emissive", (0, 1, 2))):
                if channel not in q or channel not in c:
                    continue
                pos = i * 3
                diff = sum(abs(q[channel][pos + k] - c[channel][pos + k])
                           for k in indices) / (255 * len(indices))
                sums[kind] += overlap * (1 - diff)
                weights[kind] += overlap
            if "normal" in q and "normal" in c:
                pos = i * 3
                va = tuple((q["normal"][pos + k] / 255) * 2 - 1 for k in range(3))
                vb = tuple((c["normal"][pos + k] / 255) * 2 - 1 for k in range(3))
                na, nb = math.sqrt(sum(v * v for v in va)), math.sqrt(sum(v * v for v in vb))
                cosine = sum(x * y for x, y in zip(va, vb)) / max(1e-8, na * nb)
                sums["normal"] += overlap * clamp((cosine + 1) / 2)
                weights["normal"] += overlap
    result = {"shape": intersections / unions if unions else 0.0}
    for kind in sums:
        channel = {"color": "albedo", "normal": "normal", "roughness": "orm",
                   "metallic": "orm", "occlusion": "orm", "emissive": "emissive"}[kind]
        available = any(channel in encoded and channel in candidate["features"].get(view, {})
                        for view, encoded in query["features"].items())
        result[kind] = (clamp(sums[kind] / weights[kind]) if weights[kind] > 0
                        else 0.0 if available else None)
    return result


def relative_sizes(a, b):
    if a is None or b is None:
        return None
    if len(a) != 3 or len(b) != 3:
        raise SearchError("bounds.size must contain three dimensions")
    if any(not math.isfinite(float(x)) or float(x) < 0 for x in (*a, *b)):
        raise SearchError("bounds.size must be nonnegative finite dimensions")
    scale = max((*a, *b, 1e-3))
    floor = scale * 0.001  # flat meshes compare meaningfully without divide-by-zero
    return [max(float(x), floor) / max(float(y), floor) for x, y in zip(a, b)]


def size_score(query, candidate):
    qa = (query.get("bounds") or {}).get("size")
    cb = (candidate.get("bounds") or {}).get("size")
    ratios = relative_sizes(qa, cb)
    if ratios is None:
        return None, None
    factors = [max(r, 1 / r) for r in ratios]
    return clamp(2 ** (-mean([math.log2(r) for r in factors]))), max(factors)


def weights_from_string(spec):
    weights = dict(DEFAULT_WEIGHTS)
    if spec:
        for token in spec.split(","):
            if "=" not in token:
                raise SearchError("weights must be comma-separated dimension=nonnegative-value")
            key, val = token.strip().split("=", 1)
            if key not in DIMENSIONS:
                raise SearchError(f"unknown weight dimension {key!r}; use {', '.join(DIMENSIONS)}")
            try:
                number = float(val)
            except ValueError as exc:
                raise SearchError(f"invalid weight for {key}") from exc
            if not math.isfinite(number) or number < 0:
                raise SearchError("weights must be finite and nonnegative")
            weights[key] = number
    if not any(weights.values()):
        raise SearchError("at least one dimension weight must be positive")
    return weights


def search(doc, query, limit=10, weights=None, size_ratio=2.0, exclude_source=None):
    """Exact linear ranking; scores are relative likeness, not probabilities."""
    if limit < 1 or size_ratio < 0 or (0 < size_ratio < 1):
        raise SearchError("limit must be positive and size ratio must be 0 or >= 1")
    weights = dict(DEFAULT_WEIGHTS) if weights is None else weights
    found = []
    for candidate in doc["assets"]:
        if exclude_source is not None and candidate["source"] == exclude_source:
            continue
        size, largest_ratio = size_score(query, candidate)
        if size_ratio and largest_ratio is not None and largest_ratio > size_ratio + 1e-9:
            continue
        parts = visual_scores(query, candidate, doc["grid"])
        parts["size"] = size
        active = [(name, w) for name, w in weights.items() if w > 0 and parts.get(name) is not None]
        if not active:
            raise SearchError("no weighted similarity dimensions exist for this query")
        total_weight = sum(w for _, w in active)
        score = 100 * sum(w * parts[name] for name, w in active) / total_weight
        found.append({"source": candidate["source"], "geometry_guid": candidate.get("geometry_guid", ""),
                      "sha256": candidate.get("sha256", ""),
                      "score": round(score, 2),
                      "dimensions": {name: None if parts.get(name) is None else round(100 * parts[name], 2)
                                     for name in DIMENSIONS},
                      "bounds": candidate.get("bounds"), "materials": candidate.get("materials", [])})
    found.sort(key=lambda item: (-item["score"], item["source"]))
    return [dict(rank=i + 1, **item) for i, item in enumerate(found[:limit])]


def match_asset(doc, needle):
    rows = [asset for asset in doc["assets"]
            if needle in (asset["source"], asset.get("geometry_guid"), asset.get("sha256"))]
    if not rows:
        # Exact path matching without depending on CWD of index creation.
        rows = [asset for asset in doc["assets"] if Path(asset["source"]).name == needle]
    if not rows:
        raise SearchError(f"no indexed asset matching {needle!r}")
    if len(rows) > 1:
        raise SearchError(f"ambiguous asset {needle!r}; use the full source path or unique GUID")
    return rows[0]


