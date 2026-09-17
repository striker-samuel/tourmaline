#!/usr/bin/env python3
# tools/rizom_uv2.py -- cook lightmap UV2 (glTF TEXCOORD_1) with RizomUV.
#
# Per mesh primitive the tool does a fileless round trip through a running
# RizomUV instance (RizomUVLink, ZMQ IPC -- no transient OBJ hits the disk):
#
#   POSITION/indices/TEXCOORD_0
#     -> Data-mode Load  (UV0 corners carry the island/cut structure)
#     -> Unfold          (re-flatten islands; no border intersections,
#                         no triangle flips)
#     -> Pack            (lightmap settings: SET_3D_AREA = uniform texel
#                         density across islands, BEST_FIT into the unity
#                         box, 90-deg pre-orient, no fine rotation)
#     -> Save Data.UseImportedUVWPolygons (per-vertex order preserved: the
#                         returned list is 1:1 with the glTF vertex order)
#     -> TEXCOORD_1 accessor appended to the glTF + <stem>_uv2.bin buffer
#
# Primitives RizomUV rejects (or that lack UV0) are skipped: PakCooker's
# authored-chart detection (see GenerateLightmapCharts, authoredUv2) falls
# them back to the built-in planar unwrapper at cook time.
#
# Usage:
#   py tools/rizom_uv2.py --gltf F:/content/models/Bistro/bistro.gltf
#   py tools/rizom_uv2.py --gltf <...> --limit 5        # smoke a few prims
#   py tools/rizom_uv2.py --gltf <...> --only 239,221   # mesh indices
#   py tools/rizom_uv2.py --gltf <...> --force           # redo existing UV2
#
# RizomUV: if no listener is found on --port, one is launched
# (rizomuv_vs.exe -id <port>). The instance is left running afterwards
# (startup is slow; subsequent runs reuse it).

import argparse
import json
import math
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

RIZOMUV_DIR = Path(r"C:\Program Files\Rizom Lab\RizomUV 2025.0")
# VS edition exe directly (rizomuv.exe is a launcher/chooser; spawning it
# can sit on a dialog instead of opening the -id link port).
RIZOMUV_EXE = RIZOMUV_DIR / "rizomuv_vs.exe"
LINK_DIR = RIZOMUV_DIR / "RizomUVLink"

sys.path.append(str(LINK_DIR))
from RizomUVLink import CRizomUVLink, CZEx  # noqa: E402


def exe(link, task, params, timeout_ms):
    # Bypass the 2s default in CRizomUVLinkBase.Execute: big meshes take
    # minutes in Unfold/Pack.
    return link.rizomuv.Execute(task, params, timeout_ms)


def connect_or_launch(port):
    link = CRizomUVLink()
    try:
        link.Connect(port)
        ver = link.rizomuv.Execute("Get", "Vars.Infos.Version.Full", 8000)
        print("[rizom] connected to running instance: %s" % ver)
        return link
    except CZEx:
        pass
    if not RIZOMUV_EXE.exists():
        raise SystemExit("RizomUV not found: %s" % RIZOMUV_EXE)
    print("[rizom] launching %s -id %d" % (RIZOMUV_EXE, port))
    subprocess.Popen([str(RIZOMUV_EXE), "-id", str(port)], cwd=str(RIZOMUV_DIR))
    deadline = time.time() + 120.0
    while time.time() < deadline:
        try:
            link.Connect(port)
            ver = link.rizomuv.Execute("Get", "Vars.Infos.Version.Full", 5000)
            print("[rizom] instance up: %s" % ver)
            return link
        except CZEx:
            time.sleep(1.0)
    raise SystemExit("RizomUV did not come up on port %d within 120s "
                     "(check the GUI for a blocking dialog)" % port)


# ---------------------------------------------------------------------------
# glTF accessors (subset: what Blender's exporter writes)
# ---------------------------------------------------------------------------

def read_accessor(gltf, acc_idx, blobs):
    acc = gltf["accessors"][acc_idx]
    view = gltf["bufferViews"][acc["bufferView"]]
    buf = blobs[view.get("buffer", 0)]
    off = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc["count"]
    ct = acc["componentType"]
    ty = acc["type"]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[ty]
    if ct == 5126:  # float
        return list(struct.unpack_from("<%df" % (n * count), buf, off)), n
    if ct == 5123:  # ushort
        return list(struct.unpack_from("<%dH" % (n * count), buf, off)), n
    if ct == 5125:  # uint
        return list(struct.unpack_from("<%dI" % (n * count), buf, off)), n
    raise ValueError("unsupported accessor componentType %d" % ct)


def clean_triangles(positions, indices):
    """Drop degenerate triangles and unreferenced vertices; returns
    (verts, tris, old_to_new) where verts/tris index the compacted set and
    old_to_new maps original glTF vertex id -> compacted id (-1 unused).
    RizomUV's triangle-tessellation import requires no duplicate-index
    polygons and no isolated vertices."""
    vcount = len(positions) // 3
    tris = []
    for t in range(len(indices) // 3):
        a, b, c = indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]
        if a == b or b == c or a == c:
            continue
        ax, ay, az = positions[a * 3:a * 3 + 3]
        bx, by, bz = positions[b * 3:b * 3 + 3]
        cx, cy, cz = positions[c * 3:c * 3 + 3]
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        wx, wy, wz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        if wx * wx + wy * wy + wz * wz < 1e-24:
            continue  # zero-area
        tris.append((a, b, c))
    old_to_new = [-1] * vcount
    used = sorted({v for tri in tris for v in tri})
    for new, old in enumerate(used):
        old_to_new[old] = new
    verts = []
    for old in used:
        verts.extend(positions[old * 3:old * 3 + 3])
    tris_flat = []
    for a, b, c in tris:
        tris_flat.extend((old_to_new[a], old_to_new[b], old_to_new[c]))
    return verts, tris_flat, old_to_new, used


def scatter_uv2(coords, old_to_new, used, vcount):
    """RizomUV's CoordsUVW (compacted import order) -> per-original-vertex
    (u, v) pairs. Vertices RizomUV never saw (dropped degenerate corners)
    keep (0, 0); they belong to no renderable triangle."""
    out = [0.0] * (vcount * 2)
    n = len(used)
    if len(coords) != n * 3:
        return None
    for new in range(n):
        old = used[new]
        out[old * 2] = coords[new * 3]
        out[old * 2 + 1] = coords[new * 3 + 1]
    return out


def scatter_corners(coords, poly_uv, tris_flat, used, vcount,
                    tol=1e-4):
    """Per-triangle-corner readback (robust to RizomUV's uv welding of
    coincident uv verts, e.g. stacked shells): PolyUVWIDs (import poly
    order) indexes CoordsUVW; corners scatter back to their original
    glTF vertices. Returns None when the corner map is inconsistent
    (a vertex's corners disagree: the UV topology changed under us)."""
    out = [0.0] * (vcount * 2)
    seen = [False] * vcount
    for t in range(len(tris_flat) // 3):
        for k in range(3):
            compacted = tris_flat[t * 3 + k]
            old = used[compacted]
            ui = poly_uv[t * 3 + k]
            u = coords[ui * 3]
            v = coords[ui * 3 + 1]
            if seen[old]:
                du = abs(out[old * 2] - u)
                dv = abs(out[old * 2 + 1] - v)
                if du > tol or dv > tol:
                    return None
            else:
                out[old * 2] = u
                out[old * 2 + 1] = v
                seen[old] = True
    return out


def classify_veg_meshes(gltf, keywords):
    """mesh index -> matched keyword for meshes inside vegetation-like
    subtrees. A node's FULL ancestor chain is checked with word-boundary
    keyword matching (case-insensitive: 'tree' hits Linden_Tree_Mesh,
    not StreetSign_mesh). Foliage leaf cards are the worst lightmap
    targets in the scene and take unreasonably long to pack in RizomUV,
    so they are skipped by default (PakCooker --lm-exclude must
    carry the same list to keep them out of the bake)."""
    if not keywords:
        return {}
    nodes = gltf.get("nodes", [])
    parent = {}
    for ni, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = ni
    pats = [(kw, re.compile(r"(?<![a-z])%s(?![a-z])" % re.escape(kw)))
            for kw in keywords]
    mesh_hit = {}
    for ni, n in enumerate(nodes):
        if "mesh" not in n:
            continue
        node = ni
        while node is not None:
            name = nodes[node].get("name", "").lower()
            for kw, pat in pats:
                if pat.search(name):
                    mesh_hit.setdefault(n["mesh"], kw)
                    break
            node = parent.get(node)
    return mesh_hit


def density_report(positions, indices, uv2, samples=512):
    """Uniform-texel-density sanity check: |uv area| / |3d area| per
    sampled triangle. A misaligned reintroduction (uv2 not matching the
    vertex order) shows up as a wild spread. Returns (p10, p50, p90)."""
    ratios = []
    tricount = len(indices) // 3
    if tricount == 0:
        return (1.0, 1.0, 1.0)
    step = max(1, tricount // samples)
    for t in range(0, tricount, step):
        ia, ib, ic = indices[t * 3:t * 3 + 3]
        p = positions
        ux, uy, uz = p[ib*3]-p[ia*3], p[ib*3+1]-p[ia*3+1], p[ib*3+2]-p[ia*3+2]
        vx, vy, vz = p[ic*3]-p[ia*3], p[ic*3+1]-p[ia*3+1], p[ic*3+2]-p[ia*3+2]
        a3 = 0.5 * math.sqrt((uy*vz-uz*vy)**2 + (uz*vx-ux*vz)**2 + (ux*vy-uy*vx)**2)
        if a3 < 1e-12:
            continue
        ua, va = uv2[ia*2], uv2[ia*2+1]
        ub, vb = uv2[ib*2], uv2[ib*2+1]
        uc, vc = uv2[ic*2], uv2[ic*2+1]
        auv = 0.5 * abs((ub-ua)*(vc-va) - (uc-ua)*(vb-va))
        if auv < 1e-14:
            continue
        ratios.append(auv / a3)
    if not ratios:
        return (1.0, 1.0, 1.0)
    ratios.sort()

    def pct(p):
        return ratios[min(len(ratios) - 1, int(p * len(ratios)))]
    return (pct(0.10), pct(0.50), pct(0.90))


# ---------------------------------------------------------------------------
# RizomUV round trip for one primitive
# ---------------------------------------------------------------------------

UNFOLD_PARAMS = {
    "BorderIntersections": True,  # never let an island self-overlap
    "TriangleFlips": True,        # never let an island invert
}
# NOTE: RizomUV's Pack fits the layout into the loaded scene's tile box,
# which spans the input UV0 range (bistro UV0s cover many UDIM tiles), so
# the packed layout is uniform-density but lives in an ARBITRARY box.
# normalize_layout below re-fits it into [pad, 1-pad] with a UNIFORM
# scale+translate (preserves the pack's texel-density uniformity exactly).
PACK_PARAMS = {
    "Translate": True,
    # E_PACK_SCALING_MODE_SET_3D_AREA: island UV area = island 3D area ->
    # uniform texel density across islands (the lightmap requirement).
    "Scaling.Mode": 1,
    # E_PACK_LAYOUT_SCALING_MODE_BEST_FIT: one uniform scale so the whole
    # layout fills the tile box (uniformity preserved).
    "LayoutScalingMode": 2,
    # E_PACK_ROTATE_MODE_AUTO: 90-deg pre-orientation for tight packing;
    # Rotate.Step 0 disables fine rotation (speed on big meshes).
    "Rotate.Mode": 1,
    "Rotate.Step": 0.0,
    # Inter-island separation in the pack's output units (shrinks with the
    # normalization below, so keep it generous).
    "PaddingSize": 0.02,
    "Resolution": 1024,
    "MaxMutations": 64,
}


def normalize_layout(uv2, pad):
    """Uniform scale+translate of the whole layout so its bbox fits
    [pad, 1-pad]^2 (uniform transform: density relationships untouched).
    Operates in place on the flat (u, v) list. Returns (min,max) range."""
    us = uv2[0::2]
    vs = uv2[1::2]
    umin, umax = min(us), max(us)
    vmin, vmax = min(vs), max(vs)
    ex = max(umax - umin, 1e-9)
    ey = max(vmax - vmin, 1e-9)
    s = (1.0 - 2.0 * pad) / max(ex, ey)
    ox = pad - umin * s + ((1.0 - 2.0 * pad) - ex * s) * 0.5
    oy = pad - vmin * s + ((1.0 - 2.0 * pad) - ey * s) * 0.5
    for i in range(0, len(uv2), 2):
        uv2[i] = uv2[i] * s + ox
        uv2[i + 1] = uv2[i + 1] * s + oy
    return (min(uv2[0::2]), max(uv2[0::2]), min(uv2[1::2]), max(uv2[1::2]))


def cook_prim(link, positions, indices, uv0, timeout_ms, pad):
    verts, tris_flat, old_to_new, used = clean_triangles(positions, indices)
    if not tris_flat:
        return None, "no non-degenerate triangles", None
    ntri = len(tris_flat) // 3
    nvert = len(used)
    uvw0 = []
    for new in range(nvert):
        old = used[new]
        uvw0.extend((uv0[old * 2], uv0[old * 2 + 1], 0.0))
    load = {
        "Data.PolySizes": [3] * ntri,
        "Data.PolyXYZIDs": tris_flat,
        "Data.CoordsXYZ": verts,
        "Data.PolyUVWIDs": tris_flat,   # uv index == vertex index (glTF)
        "Data.CoordsUVW": uvw0,
        "__Focus": False,
    }
    exe(link, "Load", load, timeout_ms)
    exe(link, "Unfold", UNFOLD_PARAMS, timeout_ms)
    exe(link, "Pack", PACK_PARAMS, timeout_ms)
    # Readback: prefer the explicit per-corner map (PolyUVWIDs indexes
    # CoordsUVW in import poly order) — robust to RizomUV welding
    # coincident uv verts (stacked shells); fall back to the import-order
    # shortcut when the corner map is absent and the counts match.
    ntri_c = ntri
    out = exe(link, "Save", {"Data": {}}, timeout_ms)
    d = out.get("Data", {}) if isinstance(out, dict) else {}
    coords = d.get("CoordsUVW") if isinstance(d, dict) else None
    poly_uv = d.get("PolyUVWIDs") if isinstance(d, dict) else None
    uv2 = None
    if coords and poly_uv and len(poly_uv) == ntri_c * 3 \
            and len(coords) % 3 == 0:
        uv2 = scatter_corners(coords, poly_uv, tris_flat, used,
                              len(positions) // 3)
    if uv2 is None:
        out = exe(link, "Save",
                  {"Data": {}, "Data.UseImportedUVWPolygons": True},
                  timeout_ms)
        d = out.get("Data", {}) if isinstance(out, dict) else {}
        coords = d.get("CoordsUVW") if isinstance(d, dict) else None
        if coords:
            uv2 = scatter_uv2(coords, old_to_new, used,
                              len(positions) // 3)
    if uv2 is None:
        got = (len(coords) // 3) if coords else 0
        return None, ("readback mismatch (got %d uv verts)" % got), None
    urange = normalize_layout(uv2, pad)
    return uv2, None, urange


# ---------------------------------------------------------------------------
# glTF writeback
# ---------------------------------------------------------------------------

class Uv2Writer:
    def __init__(self, gltf, bin_path):
        self.gltf = gltf
        self.bin_path = bin_path
        self.blob = bytearray()
        self.buf_index = None
        self.views = []      # (prim_key, bufferView idx, accessor idx)
        self.stats = []      # (prim_key, n, umin, vmin, umax, vmax)

    def add(self, key, uv2_pairs):
        n = len(uv2_pairs) // 2
        data = struct.pack("<%df" % (n * 2), *uv2_pairs)
        off = len(self.blob)
        self.blob.extend(data)
        bv = {
            "buffer": self.buf_index,
            "byteOffset": off,
            "byteLength": len(data),
            "target": 34962,
        }
        self.gltf["bufferViews"].append(bv)
        bvi = len(self.gltf["bufferViews"]) - 1
        us = uv2_pairs[0::2]
        vs = uv2_pairs[1::2]
        acc = {
            "bufferView": bvi,
            "componentType": 5126,
            "count": n,
            "type": "VEC2",
            "min": [min(us), min(vs)],
            "max": [max(us), max(vs)],
        }
        self.gltf["accessors"].append(acc)
        ai = len(self.gltf["accessors"]) - 1
        self.views.append((key, bvi, ai))
        self.stats.append((key, n, min(us), min(vs), max(us), max(vs)))

    def attach_buffer(self, uri, existing_bytes=b""):
        # Resume: previously cooked primitives keep their accessors into
        # the SAME buffer, so the new run appends after the existing blob
        # (write_bytes would otherwise truncate their data).
        self.blob = bytearray(existing_bytes)
        self.buf_index = len(self.gltf.get("buffers", []))
        self.gltf.setdefault("buffers", []).append({
            "uri": uri,
            "byteLength": 0,  # finalized in flush
        })
        self.buf_uri = uri

    def flush(self, gltf_path):
        # Pad to 4-byte multiple, update buffer length, write files.
        while len(self.blob) % 4:
            self.blob.extend(b"\0")
        self.gltf["buffers"][self.buf_index]["byteLength"] = len(self.blob)
        self.bin_path.write_bytes(bytes(self.blob))
        gltf_path.write_text(json.dumps(self.gltf, indent=1),
                             encoding="utf-8")
        # Wire TEXCOORD_1 into the primitives (after a successful flush so
        # interrupted runs never reference unwritten buffer data).
        return len(self.blob)


def main():
    ap = argparse.ArgumentParser(
        description="Cook glTF TEXCOORD_1 lightmap UVs with RizomUV.")
    ap.add_argument("--gltf", required=True)
    ap.add_argument("--port", type=int, default=50100)
    ap.add_argument("--limit", type=int, default=0,
                    help="process at most N un-cooked primitives (test)")
    ap.add_argument("--only", default="",
                    help="comma-separated mesh indices to cook (test)")
    ap.add_argument("--force", action="store_true",
                    help="re-cook primitives that already have TEXCOORD_1")
    ap.add_argument("--exclude", "--exclude-veg", dest="exclude",
                    default="tree,linden,linde,ivy,foliage,leaves,branch,canopy",
                    help="comma-separated node-name keywords whose subtrees "
                         "are skipped (word-boundary match); '' disables")
    ap.add_argument("--exclude-small", type=float, default=0.0,
                    help="skip primitives whose 3D bbox diagonal is under "
                         "this size (local units, ~meters); 0 disables. "
                         "Small props are probe-lit anyway; skipping them "
                         "here saves RizomUV time (the cook's "
                         "--lm-exclude-small must carry the same gate).")
    ap.add_argument("--timeout-per-1k-tris", type=float, default=8.0,
                    help="call timeout seconds per 1000 triangles (min 60)")
    ap.add_argument("--pad", type=float, default=0.001,
                    help="layout margin inside [0,1]^2 after normalization")
    args = ap.parse_args()

    gltf_path = Path(args.gltf)
    gltf = json.loads(gltf_path.read_text(encoding="utf-8"))
    veg = classify_veg_meshes(
        gltf, [k.strip().lower() for k in args.exclude.split(",")
               if k.strip()])
    if veg:
        print("[rizom-uv2] excluding %d name-matched meshes: %s"
              % (len(veg), sorted(set(veg.values()))), flush=True)
    if args.exclude_small > 0.0:
        print("[rizom-uv2] small-prop gate: %.3g units bbox diagonal"
              % args.exclude_small, flush=True)
    blobs = []
    for b in gltf.get("buffers", []):
        uri = b.get("uri", "")
        if uri and not uri.startswith("data:"):
            blobs.append((gltf_path.parent / uri).read_bytes())
        else:
            blobs.append(b"")

    only = {int(x) for x in args.only.split(",") if x.strip()}
    writer = Uv2Writer(gltf, gltf_path.parent / (gltf_path.stem + "_uv2.bin"))

    # Which buffer holds the new UV2 blob: reuse the _uv2 buffer entry
    # from a previous (partial) run if present, seeding the blob with its
    # current bytes so resumed appends never truncate cooked data.
    uv2_uri = gltf_path.stem + "_uv2.bin"
    uv2_bin = gltf_path.parent / uv2_uri
    existing = uv2_bin.read_bytes() if uv2_bin.exists() else b""
    for bi, b in enumerate(gltf.get("buffers", [])):
        if b.get("uri") == uv2_uri:
            writer.buf_index = bi
            writer.blob = bytearray(existing)
            break
    else:
        writer.attach_buffer(uv2_uri, existing)

    todo = []
    for mi, mesh in enumerate(gltf.get("meshes", [])):
        for pi, prim in enumerate(mesh.get("primitives", [])):
            attrs = prim.get("attributes", {})
            if "POSITION" not in attrs or "indices" not in prim:
                continue
            if "TEXCOORD_0" not in attrs:
                print("[skip] mesh %d prim %d: no TEXCOORD_0" % (mi, pi))
                continue
            if "TEXCOORD_1" in attrs and not args.force:
                continue
            if mi in veg:
                continue
            if only and mi not in only:
                continue
            todo.append((mi, pi, prim))

    if args.limit:
        todo = todo[:args.limit]
    if not todo:
        print("[rizom-uv2] nothing to do (%d meshes scanned)"
              % len(gltf.get("meshes", [])))
        return 0

    print("[rizom-uv2] %d primitives to cook (%s)"
          % (len(todo), gltf_path))
    link = connect_or_launch(args.port)

    t0 = time.time()
    done = failed = skipped_small = 0
    for order, (mi, pi, prim) in enumerate(todo):
        prim_t0 = time.time()
        attrs = prim["attributes"]
        name = gltf["meshes"][mi].get("name", "")
        positions, _ = read_accessor(gltf, attrs["POSITION"], blobs)
        indices, _ = read_accessor(gltf, prim["indices"], blobs)
        uv0, _ = read_accessor(gltf, attrs["TEXCOORD_0"], blobs)
        if args.exclude_small > 0.0:
            xs = positions[0::3]
            ys = positions[1::3]
            zs = positions[2::3]
            diag = math.sqrt((max(xs) - min(xs)) ** 2
                             + (max(ys) - min(ys)) ** 2
                             + (max(zs) - min(zs)) ** 2)
            if diag < args.exclude_small:
                skipped_small += 1
                continue
        if len(uv0) != len(positions) // 3 * 2:
            print("[fail] mesh %d prim %d '%s': uv0/position count mismatch"
                  % (mi, pi, name), flush=True)
            failed += 1
            continue
        ntri = len(indices) // 3
        timeout_ms = int(max(60.0, ntri / 1000.0
                             * args.timeout_per_1k_tris) * 1000)
        try:
            uv2, err, urange = cook_prim(link, positions, indices, uv0,
                                         timeout_ms, args.pad)
        except CZEx as ex:
            uv2, err, urange = None, "rizom: %s" % str(ex).strip()[:200], None
        except Exception as ex:  # never let one bad prim kill the run
            uv2, err, urange = None, "%s: %s" % (type(ex).__name__, ex), None
        prim_dt = time.time() - prim_t0
        if uv2 is None:
            print("[fail] mesh %d prim %d '%s': %s"
                  % (mi, pi, name, err), flush=True)
            failed += 1
            continue
        p10, p50, p90 = density_report(positions, indices, uv2)
        writer.add((mi, pi), uv2)
        done += 1
        elapsed = time.time() - t0
        pct = 100.0 * (order + 1) / len(todo)
        avg = elapsed / (order + 1)
        eta = avg * (len(todo) - order - 1)
        print("[%d/%d %5.1f%%] mesh %d prim %d '%s': %d tris ok "
              "(this %s, elapsed %s, ETA %s, fails %d) "
              "[density p10/p50/p90 = %.3g/%.3g/%.3g, "
              "range u %.3f..%.3f v %.3f..%.3f]"
              % (order + 1, len(todo), pct, mi, pi, name, ntri,
                 fmt_span(prim_dt), fmt_span(elapsed), fmt_span(eta), failed,
                 p10, p50, p90,
                 urange[0], urange[1], urange[2], urange[3]), flush=True)
        # Checkpoint every 25 prims (and let a previous TEXCOORD_1 get
        # overwritten atomically per flush).
        if done % 25 == 0:
            writer.flush(gltf_path)
            _attach_texcoord1(gltf, writer)
            gltf_path.write_text(json.dumps(gltf, indent=1),
                                 encoding="utf-8")
            print("  [ckpt] checkpointed %d/%d (%s), uv2 blob %.1f MB"
                  % (done, len(todo), fmt_span(time.time() - t0),
                     len(writer.blob) / 1e6), flush=True)
    writer.flush(gltf_path)
    _attach_texcoord1(gltf, writer)
    gltf_path.write_text(json.dumps(gltf, indent=1), encoding="utf-8")
    dt = time.time() - t0
    print("[rizom-uv2] done: %d cooked, %d failed, %d small-skipped, "
          "%.1f min, uv2 blob %.1f MB"
          % (done, failed, skipped_small, dt / 60.0,
             len(writer.blob) / 1e6), flush=True)
    return 1 if failed else 0


def fmt_span(seconds):
    seconds = max(0.0, float(seconds))
    if seconds < 90:
        return "%.0fs" % seconds
    mins = seconds / 60.0
    if mins < 90:
        return "%.1fm" % mins
    return "%.1fh" % (mins / 60.0)


def _attach_texcoord1(gltf, writer):
    for (mi, pi), bvi, ai in writer.views:
        prim = gltf["meshes"][mi]["primitives"][pi]
        prim["attributes"]["TEXCOORD_1"] = ai


if __name__ == "__main__":
    sys.exit(main())
