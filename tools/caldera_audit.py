#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""caldera_audit: offline content audit for the Activision Caldera USD dataset.

Caldera (https://github.com/Activision/caldera) is an OpenUSD scene containing
most of the Call of Duty: Warzone map geometry: ~17.5M primitives, >2B points,
2x2 miles, Z-up, inch units, no textures/materials. It uses PointInstancers for
vegetation/clutter, `districtLod` variants (proxy vs full), payloads and
instance proxies. Tourmaline's OpenUSD importer (`tools/gltf_import/src/usd_import.cpp`)
currently fails closed on PointInstancers, instance proxies, subdivision
meshes and connected shader graphs.

This tool is the content model that precedes importer/streamer changes. It
reports, without composing the whole map in memory:

  - prototype counts (distinct Mesh prims) and estimated vertex/index bytes
  - instance counts (PointInstancer protoIndices length, plus direct meshes)
  - per-district content (files and prims grouped by district directory)
  - unsupported USD schemas, with a disposition (supported / ignored /
    must-implement), so nothing is silently discarded
  - estimated per-cell working set, projected onto Tourmaline's 256 m root
    cell grid (floor semantics, matching Engine/WorldPartition)

Two modes:

  inventory  -- fast, file-level, does NOT compose stages. Walks the source
                tree, reads each USD layer's prim specs, and classifies every
                authored prim type. Safe to run over the whole repo.

  compose    -- fully composes ONE stage (a district .usd, the root
                caldera.usda, or any sub-scene), resolves payloads/variants/
                references, and reports detailed geometry/instance/byte and
                per-cell working-set numbers. Use on a per-district basis;
                composing the entire island with all payloads loaded needs a
                large amount of RAM.

Dependencies: `pip install usd-core` (the OpenUSD Python bindings). No Vulkan,
Qt, or Tourmaline build is required.

Examples:
  python tools/caldera_audit.py inventory G:/caldera_test/caldera --json
  python tools/caldera_audit.py compose G:/caldera_test/caldera/caldera.usda
  python tools/caldera_audit.py compose \
      G:/caldera_test/caldera/map_source/prefabs/br/wz_vg/mp_wz_island/season_4/map_capital.usd \
      --load-payloads --cell-size 256 --json
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

try:
    from pxr import Sdf, Usd, UsdGeom, UsdShade
except ImportError:  # pragma: no cover - exercised only when usd-core is absent
    sys.exit("caldera_audit requires OpenUSD Python bindings (`pip install usd-core`)")


# ---------------------------------------------------------------------------
# Unit / grid constants. Tourmaline's world grid is metre-based; Caldera is
# authored in inches, Z-up. The importer (usd_import.cpp) converts Z-up to
# Y-up with basis {x, -z, y} * metersPerUnit, so the horizontal cell plane in
# Tourmaline (XZ) is Caldera's (X, Y) horizontal plane.
# ---------------------------------------------------------------------------
DEFAULT_METERS_PER_UNIT = 0.01  # USD default is 1 cm; Caldera authors inches
ROOT_CELL_M = 256.0
SUBCELL_M = 64.0

USD_EXTENSIONS = {".usd", ".usda", ".usdc", ".usdz"}

# Prim types the current Tourmaline importer handles as scene content or
# resources. Everything not listed here is reported as "unsupported" so the
# audit never silently discards a schema. See docs/openusd_import.md.
SUPPORTED_SCHEMAS = {
    "Xform", "Scope", "Mesh", "Camera", "Material", "Shader", "GeomSubset",
    "Sphere", "Cube", "Cylinder", "Cone", "Capsule", "Points", "BasisCurves",
    "NurbsCurves", "NurbsPatch", "BasisCurves", "Rect", "Curves",
    "",  # untyped grouping prims
}

# Schemas the importer reads as resources (not scene nodes) and therefore
# reports separately rather than as renderable geometry.
RESOURCE_SCHEMAS = {"Material", "Shader", "GeomSubset"}

# Simple Gprims. Caldera includes these as "alternative, usually invisible
# representations used in other calculations" (collision shapes, lighting
# volumes) -- NOT primary renderable geometry. The audit keeps them separate
# from Mesh prototypes so the renderable working set is not inflated.
PRIMITIVE_SHAPE_SCHEMAS = {"Sphere", "Cube", "Cylinder", "Cone", "Capsule", "Rect"}
CURVE_SCHEMAS = {"BasisCurves", "NurbsCurves", "NurbsPatch", "Curves"}

# Schemas that currently FAIL the importer and therefore need implementation
# (or an explicit documented omission) before Caldera can be imported. The
# audit separates these from "unknown but probably ignorable" so the
# PointInstancer work is visible at a glance.
MUST_IMPLEMENT_SCHEMAS = {"PointInstancer"}

# Prim types that are structural (not geometry, not resources) and expected to
# exist in a scene; counted for completeness but not flagged as unsupported.
STRUCTURAL_SCHEMAS = {"Xform", "Scope", ""}


def human_bytes(n: float) -> str:
    """Format a byte count with a human unit, decimal GB/MB/KB."""
    if n >= 1e9:
        return f"{n / 1e9:.2f} GB"
    if n >= 1e6:
        return f"{n / 1e6:.2f} MB"
    if n >= 1e3:
        return f"{n / 1e3:.2f} KB"
    return f"{n:.0f} B"


def cell_key(world_x: float, world_y: float, meters_per_unit: float,
             cell_size: float) -> tuple[int, int]:
    """Project a Z-up world position (stage units) onto Tourmaline's metre grid.

    world_x/world_y are the HORIZONTAL axes of the Z-up source (Caldera X, Y);
    they map to Tourmaline XZ. Floor semantics match Engine/WorldPartition's
    `GetWorldCell` (a -1 m coordinate lands in cell -1).
    """
    mx = world_x * meters_per_unit
    mz = world_y * meters_per_unit
    return (math.floor(mx / cell_size), math.floor(mz / cell_size))


def prim_type(prim_spec_or_prim) -> str:
    """Return the authored type name for either an Sdf.PrimSpec or Usd.Prim."""
    type_name = getattr(prim_spec_or_prim, "typeName", None)
    if type_name is None and hasattr(prim_spec_or_prim, "GetTypeName"):
        type_name = prim_spec_or_prim.GetTypeName()
    return str(type_name) if type_name else ""


def district_of(path: Path, root: Path) -> str:
    """Derive a stable district label from a source path.

    Caldera organises prefabs under map_source/prefabs/br/wz_vg/mp_wz_island/
    <district>/<...>. Districts include `season_4` (capital/airfield/beachhead/
    phosphate_mine), `commercial` (hotel/restaurant), `superterrrain`
    (terrain), and `map_tile_n` (power_station). Anything not under that
    branch falls back to its top-level directory (map_source, layers, assets).
    """
    try:
        rel = path.resolve().relative_to(root.resolve())
    except ValueError:
        rel = path
    parts = list(rel.parts)
    if "mp_wz_island" in parts:
        idx = parts.index("mp_wz_island")
        # The district is the first component after mp_wz_island, or the file
        # itself when the prefab sits directly under that dir.
        if idx + 1 < len(parts):
            return parts[idx + 1]
        return "mp_wz_island"
    return parts[0] if parts else str(path)


def estimate_mesh_bytes(mesh: UsdGeom.Mesh) -> tuple[int, int, int]:
    """Return (points, faces, bytes) for a composed UsdGeom.Mesh.

    bytes is the SOURCE vertex/index representation (positions + normals +
    st UV + face-vertex indices + face-vertex counts). It is NOT the cooked
    Tourmaline size (meshlets/quantisation compress it further), but it is a
    faithful relative working-set measure across the map.
    """
    points = mesh.GetPointsAttr().Get()
    counts = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    point_count = len(points) if points is not None else 0
    face_count = len(counts) if counts is not None else 0
    index_count = int(sum(counts)) if counts is not None else 0

    normals = mesh.GetNormalsAttr().Get()
    normal_count = len(normals) if normals is not None else 0
    st = UsdGeom.PrimvarsAPI(mesh).GetPrimvar("st")
    uv = st.Get() if st else None
    uv_count = len(uv) if uv is not None else 0

    byte_count = (
        point_count * 12  # float3 positions
        + normal_count * 12  # float3 normals (faceVarying overcounts: estimate)
        + uv_count * 8  # float2 st
        + index_count * 4  # uint32 face-vertex indices
        + face_count * 4  # uint32 face-vertex counts
    )
    return point_count, face_count, byte_count


def content_identity(prim) -> str:
    """Content identity for a composed prim: the strongest DEFINING source spec.

    Two composed prims that reference the same source asset share one identity,
    so this deduplicates reference-based placements (the Caldera building
    duplication) without hashing geometry bytes.
    """
    for spec in reversed(prim.GetPrimStack()):
        if spec.specifier == Sdf.SpecifierDef:
            return f"{spec.layer.identifier}::{spec.path}"
    return str(prim.GetPath())


def subtree_mesh_bytes(prim) -> int:
    """Total source mesh bytes under a prim (descends Xform/Scope subtrees).

    PointInstancer prototypes are Xform subtrees containing several meshes, not
    bare Mesh prims. Sum the leaf Mesh bytes; composed stage hierarchies are
    shallow, so plain recursion is safe.
    """
    if prim is None:
        return 0
    if prim.GetTypeName() == "Mesh":
        _, _, byte_count = estimate_mesh_bytes(UsdGeom.Mesh(prim))
        return byte_count
    return sum(subtree_mesh_bytes(child) for child in prim.GetChildren())


def classify_schema(type_name: str) -> str:
    """Bucket a prim type into a disposition used by the report.

    Returns one of: "mesh", "primitive_shape", "curve", "points", "camera",
    "resource", "structural", "must_implement", "unsupported". "unsupported"
    means "not in the current importer and not obviously a resource/structural
    prim" - each such type is surfaced so a human can assign a disposition
    instead of silently dropping it.
    """
    if type_name in MUST_IMPLEMENT_SCHEMAS:
        return "must_implement"
    if type_name == "Mesh":
        return "mesh"
    if type_name in PRIMITIVE_SHAPE_SCHEMAS:
        return "primitive_shape"
    if type_name in CURVE_SCHEMAS:
        return "curve"
    if type_name == "Points":
        return "points"
    if type_name == "Camera":
        return "camera"
    if type_name in RESOURCE_SCHEMAS:
        return "resource"
    if type_name in STRUCTURAL_SCHEMAS:
        return "structural"
    if type_name in SUPPORTED_SCHEMAS:
        return "supported"
    return "unsupported"


# ---------------------------------------------------------------------------
# Inventory mode: per-file, uncomposed layer inspection.
# ---------------------------------------------------------------------------
def inventory_layer(layer: Sdf.Layer) -> Counter:
    """Count authored prim specs by type across one layer (no composition).

    `Sdf.Layer.Traverse` is the C++-style form: it calls a callback with each
    `SdfPath` (there is no Pythonic `traverse` generator). Resolve each path
    back to its `Sdf.PrimSpec` to read the authored type name. This descends
    into variant-set opinions, so both the "proxy" and "full" `districtLod`
    alternatives are counted, which is the point of the authored inventory.
    """
    result: Counter = Counter()

    def visit(path: Sdf.Path) -> None:
        prim_spec = layer.GetPrimAtPath(path)
        if prim_spec is not None:
            result[prim_type(prim_spec)] += 1

    layer.Traverse(Sdf.Path("/"), visit)
    return result


def run_inventory(root: Path) -> dict:
    files: list[dict] = []
    per_district: dict[str, Counter] = defaultdict(Counter)
    schema_totals: Counter = Counter()
    disposition_totals: Counter = Counter()
    extension_totals: Counter = Counter()
    unsupported_types: Counter = Counter()

    usd_files = [p for p in sorted(root.rglob("*")) if p.suffix.lower() in USD_EXTENSIONS]

    for path in usd_files:
        district = district_of(path, root)
        ext = path.suffix.lower()
        extension_totals[ext] += 1
        record = {"path": str(path), "district": district, "ext": ext,
                  "schema_counts": {}, "unsupported": {}}
        try:
            layer = Sdf.Layer.FindOrOpen(str(path))
            if layer is None:
                record["error"] = "cannot open layer"
            else:
                counts = inventory_layer(layer)
                record["schema_counts"] = dict(sorted(counts.items()))
                record["prim_count"] = sum(counts.values())
                per_district[district] += counts
                schema_totals += counts
                for type_name, n in counts.items():
                    disposition_totals[classify_schema(type_name)] += n
                    if classify_schema(type_name) == "unsupported":
                        unsupported_types[type_name] += n
                        record["unsupported"][type_name] = n
        except Exception as exc:  # noqa: BLE001 - audit must survive one bad file
            record["error"] = f"{type(exc).__name__}: {exc}"
        files.append(record)

    return {
        "mode": "inventory",
        "root": str(root),
        "usd_file_count": len(usd_files),
        "extension_counts": dict(sorted(extension_totals.items())),
        "schema_totals": dict(sorted(schema_totals.items())),
        "disposition_totals": dict(sorted(disposition_totals.items())),
        "unsupported_types": dict(sorted(unsupported_types.items(),
                                         key=lambda kv: -kv[1])),
        "per_district": {
            d: dict(sorted(c.items())) for d, c in sorted(per_district.items())
        },
        "files": files,
    }


# ---------------------------------------------------------------------------
# Compose mode: full composition of one stage.
# ---------------------------------------------------------------------------
def apply_variants(stage: Usd.Stage, variant_specs: list[str]) -> None:
    """Select variants (each "name=value") on every prim that owns the set.

    Caldera authors a `districtLod` variant set (default "proxy", alternative
    "full") on each district prim; auditing the real geometry requires selecting
    "full". A parent variant selection re-syncs its whole subtree, so snapshot
    the lightweight tree first, then apply, then let the caller re-traverse.
    """
    for spec in variant_specs:
        name, _, value = spec.partition("=")
        if not name or not value:
            raise SystemExit(f"invalid --variant {spec!r} (expected name=value)")
        applied = 0
        for prim in list(stage.Traverse()):
            vsets = prim.GetVariantSets()
            if vsets.HasVariantSet(name):
                vsets.GetVariantSet(name).SetVariantSelection(value)
                applied += 1
        if applied == 0:
            print(f"caldera_audit: warning: variant set {name!r} not found on any prim",
                  file=sys.stderr)


def run_compose(stage_path: Path, load_payloads: bool, variants: list[str],
                cell_size: float, subcell_size: float) -> dict:
    stage = Usd.Stage.Open(str(stage_path),
                           Usd.Stage.LoadAll if load_payloads else Usd.Stage.LoadNone)
    if not stage:
        raise SystemExit(f"cannot open composed stage: {stage_path}")
    if variants:
        apply_variants(stage, variants)

    meters_per_unit = UsdGeom.GetStageMetersPerUnit(stage)
    up_axis = UsdGeom.GetStageUpAxis(stage)

    schema_totals: Counter = Counter()
    disposition_totals: Counter = Counter()
    unsupported_types: Counter = Counter()

    total_mesh_points = 0
    total_mesh_faces = 0
    total_mesh_bytes = 0

    instancer_count = 0
    total_instances = 0
    instancer_prototype_refs = Counter()
    direct_mesh_placements = 0

    subdivision_meshes = 0
    instance_proxy_prims = 0
    material_count = 0
    camera_count = 0
    points_prim_count = 0
    points_point_count = 0

    # Prototype geometry: a "prototype" is a renderable unit -- either a
    # directly placed Mesh, or a PointInstancer prototype (an Xform subtree
    # whose meshes are instanced many times). bytes_by_key maps each prototype
    # key to its total source mesh bytes, deduplicated by CONTENT identity so
    # the same building part referenced by many placements counts once.
    prototype_bytes_by_key: dict[str, int] = {}
    direct_mesh_assets: set[str] = set()

    # Per-root-cell working set, keyed (cell_x, cell_z).
    cells = defaultdict(lambda: {
        "instance_count": 0, "mesh_placements": 0, "instancer_instances": 0,
        "prototype_keys": set(),
    })

    xf_cache = UsdGeom.XformCache(Usd.TimeCode.Default())
    # "guide" (scene annotations) is excluded; default/proxy/render are the
    # geometry purposes (Caldera clutter is tagged "render").
    bbox_cache = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_, UsdGeom.Tokens.proxy, UsdGeom.Tokens.render],
    )

    # Pass 1: locate every PointInstancer and the set of prototype subtrees it
    # references. Prototype meshes must NOT be counted as direct placements.
    instancer_prims: list[Usd.Prim] = []
    prototype_roots: set[str] = set()
    for prim in stage.Traverse():
        if prim.GetTypeName() == "PointInstancer":
            instancer_prims.append(prim)
            for target in UsdGeom.PointInstancer(prim).GetPrototypesRel().GetTargets():
                prototype_roots.add(str(target))

    def under_prototype(path: str) -> bool:
        for root in prototype_roots:
            if path == root or path.startswith(root + "/"):
                return True
        return False

    # Pass 2: full accounting over the composed stage.
    for prim in stage.Traverse():
        if not prim.IsActive() or not prim.IsDefined():
            continue
        type_name = prim_type(prim)
        schema_totals[type_name] += 1
        disposition_totals[classify_schema(type_name)] += 1
        if classify_schema(type_name) == "unsupported":
            unsupported_types[type_name] += 1

        if prim.IsInstance():
            instance_proxy_prims += 1

        if type_name == "Mesh":
            mesh = UsdGeom.Mesh(prim)
            # subdivisionScheme has a schema fallback of "catmullClark", so a
            # bare .Get() lies for unauthored (plain polygon) meshes. Match the
            # importer: only an AUTHORED non-"none" scheme needs tessellation.
            scheme_attr = mesh.GetSubdivisionSchemeAttr()
            scheme = scheme_attr.Get() if scheme_attr.HasAuthoredValue() else None
            if scheme and scheme != UsdGeom.Tokens.none:
                subdivision_meshes += 1
            pts, faces, byte_count = estimate_mesh_bytes(mesh)
            path = str(prim.GetPath())
            total_mesh_points += pts
            total_mesh_faces += faces
            total_mesh_bytes += byte_count
            if under_prototype(path):
                # Shared instancer prototype geometry, not a direct placement.
                continue
            direct_mesh_placements += 1
            # Key by CONTENT identity (source asset), not composed path: the
            # same building part referenced by many placements is one asset.
            ident = content_identity(prim)
            direct_mesh_assets.add(ident)
            prototype_bytes_by_key[ident] = byte_count
            bound = bbox_cache.ComputeWorldBound(prim)
            rng = bound.ComputeCentroid()
            cx, cz = cell_key(rng[0], rng[1], meters_per_unit, cell_size)
            entry = cells[(cx, cz)]
            entry["mesh_placements"] += 1
            entry["instance_count"] += 1
            entry["prototype_keys"].add(ident)
            continue

        if type_name == "Points":
            points_prim_count += 1
            pts = UsdGeom.Points(prim).GetPointsAttr().Get()
            points_point_count += len(pts) if pts is not None else 0
        if type_name == "Material":
            material_count += 1
        if type_name == "Camera":
            camera_count += 1

    # Attribute PointInstancer instances to cells. Prototype bytes are resolved
    # once per prototype and shared, never multiplied per instance.
    for instancer_prim in instancer_prims:
        instancer = UsdGeom.PointInstancer(instancer_prim)
        instancer_count += 1
        proto_indices = instancer.GetProtoIndicesAttr().Get()
        positions = instancer.GetPositionsAttr().Get()
        n = len(proto_indices) if proto_indices is not None else 0
        total_instances += n
        prototypes = instancer.GetPrototypesRel().GetTargets()
        proto_byte_cache: dict[int, int] = {}
        for idx, target in enumerate(prototypes):
            instancer_prototype_refs[str(target)] += 1
            proto_prim = stage.GetPrimAtPath(target)
            byte_count = subtree_mesh_bytes(proto_prim) if proto_prim else 0
            proto_byte_cache[idx] = byte_count
            prototype_bytes_by_key.setdefault(str(target), byte_count)
        local_to_world = xf_cache.GetLocalToWorldTransform(instancer_prim)
        pos_list = positions if positions is not None else ()
        for i in range(min(n, len(pos_list))):
            world = local_to_world.Transform(pos_list[i])
            proto_idx = proto_indices[i]
            key = str(prototypes[proto_idx]) if proto_idx < len(prototypes) else None
            cx, cz = cell_key(world[0], world[1], meters_per_unit, cell_size)
            entry = cells[(cx, cz)]
            entry["instance_count"] += 1
            entry["instancer_instances"] += 1
            if key is not None:
                entry["prototype_keys"].add(key)

    # Finalise per-cell bytes from the deduplicated prototype key set: a
    # prototype's bytes count once per cell, not once per instance.
    for entry in cells.values():
        entry["prototype_bytes"] = sum(
            prototype_bytes_by_key[k] for k in entry["prototype_keys"])
        entry["prototype_count"] = len(entry["prototype_keys"])
        del entry["prototype_keys"]

    distinct_prototypes = len(prototype_bytes_by_key)
    distinct_prototype_bytes = sum(prototype_bytes_by_key.values())
    nonempty_cells = len(cells)
    peak_cell_bytes = max((c["prototype_bytes"] for c in cells.values()), default=0)

    return {
        "mode": "compose",
        "stage": str(stage_path),
        "meters_per_unit": meters_per_unit,
        "up_axis": str(up_axis),
        "load_payloads": load_payloads,
        "variants": list(variants),
        "cell_size_m": cell_size,
        "subcell_size_m": subcell_size,
        "schema_totals": dict(sorted(schema_totals.items())),
        "disposition_totals": dict(sorted(disposition_totals.items())),
        "unsupported_types": dict(sorted(unsupported_types.items(),
                                         key=lambda kv: -kv[1])),
        "must_implement": {
            t: schema_totals.get(t, 0) for t in sorted(MUST_IMPLEMENT_SCHEMAS)
        },
        "geometry": {
            "mesh_placements": direct_mesh_placements,
            "unique_mesh_assets": len(direct_mesh_assets),
            "dedup_ratio": round(direct_mesh_placements / len(direct_mesh_assets), 2)
                           if direct_mesh_assets else 0.0,
            "unique_mesh_bytes": distinct_prototype_bytes,
            "unique_mesh_bytes_human": human_bytes(distinct_prototype_bytes),
            "total_mesh_points": total_mesh_points,
            "total_mesh_faces": total_mesh_faces,
            "total_composed_bytes": total_mesh_bytes,
            "total_composed_bytes_human": human_bytes(total_mesh_bytes),
            "subdivision_meshes": subdivision_meshes,
            "instance_proxy_prims": instance_proxy_prims,
            "points_prims": points_prim_count,
            "points_points": points_point_count,
        },
        "instancing": {
            "instancer_count": instancer_count,
            "total_instances": total_instances,
            "prototype_refs": dict(sorted(instancer_prototype_refs.items())),
        },
        "resources": {"materials": material_count, "cameras": camera_count},
        "cells": {
            "nonempty_cells": nonempty_cells,
            "peak_cell_prototype_bytes": peak_cell_bytes,
            "peak_cell_prototype_bytes_human": human_bytes(peak_cell_bytes),
            "details": {f"{cx},{cz}": {
                "instance_count": c["instance_count"],
                "mesh_placements": c["mesh_placements"],
                "instancer_instances": c["instancer_instances"],
                "prototype_count": c["prototype_count"],
                "prototype_bytes": c["prototype_bytes"],
            } for (cx, cz), c in sorted(cells.items())},
        },
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="caldera_audit",
        description="Offline content audit for the Activision Caldera USD dataset.",
    )
    parser.add_argument("--json", action="store_true",
                        help="emit a single JSON document instead of text")
    sub = parser.add_subparsers(dest="command", required=True)

    inv = sub.add_parser("inventory", help="file-level, uncomposed source-tree audit")
    inv.add_argument("root", help="Caldera repository root (contains map_source/)")
    inv.add_argument("--json", action="store_true", default=argparse.SUPPRESS,
                     help="emit a single JSON document instead of text")

    comp = sub.add_parser("compose", help="fully compose one stage and report geometry")
    comp.add_argument("stage", help="path to a .usd/.usda/.usdc stage")
    comp.add_argument("--json", action="store_true", default=argparse.SUPPRESS,
                      help="emit a single JSON document instead of text")
    comp.add_argument("--load-payloads", action="store_true",
                      help="load all payloads (use per-district; very memory heavy for the full map)")
    comp.add_argument("--variant", action="append", default=[], metavar="NAME=VALUE",
                      help="select a variant set on every prim that has it "
                           "(e.g. --variant districtLod=full); repeatable")
    comp.add_argument("--cell-size", type=float, default=ROOT_CELL_M,
                      help=f"root cell size in metres (default {ROOT_CELL_M})")
    comp.add_argument("--subcell-size", type=float, default=SUBCELL_M,
                      help=f"subcell size in metres (default {SUBCELL_M})")

    args = parser.parse_args(argv)

    if args.command == "inventory":
        report = run_inventory(Path(args.root))
    else:
        report = run_compose(Path(args.stage), args.load_payloads, args.variant,
                             args.cell_size, args.subcell_size)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0

    _print_report(report)
    return 0


def _print_report(report: dict) -> None:
    if report["mode"] == "inventory":
        print(f"Caldera inventory: {report['root']}")
        print(f"  USD files: {report['usd_file_count']}  "
              f"{report['extension_counts']}")
        print("  Disposition totals (prim specs):")
        for disp, n in report["disposition_totals"].items():
            print(f"    {disp:14} {n}")
        if report["unsupported_types"]:
            print("  Unsupported schema types (need a disposition):")
            for t, n in report["unsupported_types"].items():
                print(f"    {t or '<untyped>':24} {n}")
        print("  Per-district prim-spec counts:")
        for d, counts in report["per_district"].items():
            total = sum(counts.values())
            print(f"    {d:20} {total:>9}  "
                  + " ".join(f"{k}={v}" for k, v in sorted(counts.items())[:5]))
        return

    g = report["geometry"]
    i = report["instancing"]
    c = report["cells"]
    print(f"Caldera compose: {report['stage']}")
    print(f"  units: {report['meters_per_unit']} m/unit  upAxis: {report['up_axis']}"
          f"  payloads: {'loaded' if report['load_payloads'] else 'authored'}")
    print("  Geometry:")
    print(f"    mesh placements       {g['mesh_placements']}")
    print(f"    unique mesh assets    {g['unique_mesh_assets']}  (dedup {g['dedup_ratio']}x)")
    print(f"    unique mesh bytes     {g['unique_mesh_bytes_human']}")
    print(f"    source points         {g['total_mesh_points']}")
    print(f"    source faces          {g['total_mesh_faces']}")
    print(f"    composed bytes        {g['total_composed_bytes_human']} (before dedup)")
    print(f"    subdivision meshes    {g['subdivision_meshes']}  (fail closed)")
    print(f"    instance proxies      {g['instance_proxy_prims']}  (fail closed)")
    print(f"    Points prims          {g['points_prims']}  ({g['points_points']} points)")
    print("  Instancing:")
    print(f"    PointInstancers       {i['instancer_count']}")
    print(f"    total instances       {i['total_instances']}")
    print("  Must implement (fail closed today):")
    for t, n in report["must_implement"].items():
        print(f"    {t:24} {n}")
    if report["unsupported_types"]:
        print("  Other unsupported schema types:")
        for t, n in report["unsupported_types"].items():
            print(f"    {t or '<untyped>':24} {n}")
    print(f"  Cell working set ({report['cell_size_m']} m root cells,"
          f" {c['nonempty_cells']} nonempty):")
    print(f"    peak cell prototype bytes  {c['peak_cell_prototype_bytes_human']}")
    top = sorted(c["details"].items(), key=lambda kv: -kv[1]["prototype_bytes"])[:8]
    print("    top cells (cx,cz : instances / direct / instanced / prototypes / bytes):")
    for key, d in top:
        print(f"      {key:>12}  {d['instance_count']:>8}  {d['mesh_placements']:>8}"
              f"  {d['instancer_instances']:>8}  {d['prototype_count']:>6}"
              f"  {human_bytes(d['prototype_bytes'])}")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
