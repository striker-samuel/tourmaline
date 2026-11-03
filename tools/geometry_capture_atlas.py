#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Offline, CPU-only triplanar capture atlas for imported .geometry assets.

Requires Pillow (python -m pip install pillow). No Vulkan, GPU, or glTF reimport
is needed. Captures the six signed directions along the three principal axes.
The per-mip JSON rects are deliberately authoritative: each mip is packed
separately, so reverse-search consumers must not assume bit-shifted offsets.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None

CHANNELS = ("albedo", "normal", "orm", "emissive", "coverage", "material_id")
# (view name, camera-right vector, camera-up vector, towards-camera vector)
VIEWS = (
    ("+X", (0, 0, -1), (0, 1, 0), (1, 0, 0)),
    ("-X", (0, 0, 1), (0, 1, 0), (-1, 0, 0)),
    ("+Y", (1, 0, 0), (0, 0, -1), (0, 1, 0)),
    ("-Y", (1, 0, 0), (0, 0, 1), (0, -1, 0)),
    ("+Z", (1, 0, 0), (0, 1, 0), (0, 0, 1)),
    ("-Z", (-1, 0, 0), (0, 1, 0), (0, 0, -1)),
)
IDENTITY = (1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0)


class GeometryError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def take(self, size: int) -> bytes:
        if size < 0 or size > len(self.data) - self.offset:
            raise GeometryError("truncated .geometry asset")
        start = self.offset
        self.offset += size
        return self.data[start:self.offset]

    def unpack(self, fmt: str):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def u32(self):
        return self.unpack("I")[0]

    def string(self):
        length = self.u32()
        if length > 1024 * 1024:
            raise GeometryError("oversized .geometry string")
        return self.take(length).decode("utf-8")

    def count(self, item_bytes=1):
        count = self.u32()
        if count > 4_000_000 or count * item_bytes > len(self.data) - self.offset:
            raise GeometryError("invalid .geometry array length")
        return count


def normalize(v):
    length = math.sqrt(sum(x * x for x in v))
    return tuple(x / length for x in v) if length > 1e-15 else (0.0, 0.0, 1.0)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def oct_decode(word, tangent=False):
    x = ((word >> 16) & 65535) / 65535 * 2 - 1 if tangent else (word & 65535) / 65535 * 2 - 1
    y = ((word >> 1) & 32767) / 32767 * 2 - 1 if tangent else (word >> 16) / 65535 * 2 - 1
    z = 1 - abs(x) - abs(y)
    if z < 0:
        x, y = (1 - abs(y)) * (1 if x >= 0 else -1), (1 - abs(x)) * (1 if y >= 0 else -1)
    return normalize((x, y, z))


def matmul(a, b):
    return tuple(sum(a[k * 4 + row] * b[col * 4 + k] for k in range(4))
                 for col in range(4) for row in range(4))


def transform(m, v):
    x, y, z = v
    w = m[3] * x + m[7] * y + m[11] * z + m[15]
    if abs(w) < 1e-12:
        raise GeometryError("non-invertible instance transform")
    return tuple((m[i] * x + m[i + 4] * y + m[i + 8] * z + m[i + 12]) / w for i in range(3))


def transform_direction(m, v):
    c0, c1, c2 = (m[0], m[1], m[2]), (m[4], m[5], m[6]), (m[8], m[9], m[10])
    a, b, c = cross(c1, c2), cross(c2, c0), cross(c0, c1)
    sign = -1 if dot(c0, a) < 0 else 1
    return normalize(tuple(sign * (v[0] * a[i] + v[1] * b[i] + v[2] * c[i]) for i in range(3)))


@dataclass
class Vertex:
    pos: tuple
    uv: tuple
    color: tuple
    normal: tuple
    tangent: tuple
    handedness: int


@dataclass
class Blob:
    vertices: list
    indices: tuple
    surfaces: list


def load_geometry(path: Path):
    """Read the base SoA meshes from the current MESH v3 .geometry format.

    LOD, meshlet, lightmap, and skin trailers are not needed for a level-0
    orthographic reference capture; they remain untouched.
    """
    data = path.read_bytes()
    r = Reader(data)
    if r.u32() != 0x4D455348 or r.u32() != 3:
        raise GeometryError(f"{path}: expected MESH v3 .geometry (reimport old assets)")
    count = r.count(8)
    parents = [r.unpack("ii")[0] for _ in range(count)]
    name_count = r.count(4)
    for _ in range(name_count):
        r.string()
    local_count = r.count(64)
    locals_ = [r.unpack("16f") for _ in range(local_count)]
    for _ in range(2):  # staticRoot and noLightmap flags
        r.take(r.count())
    if local_count != count or name_count != count:
        raise GeometryError("node array counts differ")
    instance_count = r.count(12)
    instances = [(r.u32(), r.u32(), r.string()) for _ in range(instance_count)]
    for _ in range(r.count(40)):  # light: node/type/color/4 params/flags/name
        r.take(40)
        r.string()
    for _ in range(r.count(24)):  # camera: node/projection/4 params/name
        r.take(24)
        r.string()

    blobs = []
    for _ in range(r.count(4)):
        vertex_count, index_count = r.u32(), r.u32()
        if vertex_count > 4_000_000 or index_count > 12_000_000:
            raise GeometryError("excessive .geometry vertex/index count")
        surfaces = [r.unpack("IIi") for _ in range(r.count(12))]
        r.take(24)  # aabbMin, aabbMax
        positions = [item[0:3] for item in struct.iter_unpack("<fff", r.take(vertex_count * 12))]
        shading = list(struct.iter_unpack("<IIII", r.take(vertex_count * 16)))
        lm_count = r.count(8)
        if lm_count not in (0, vertex_count):
            raise GeometryError("lightmap stream vertex mismatch")
        r.take(lm_count * 8)
        indices = tuple(item[0] for item in struct.iter_unpack("<I", r.take(index_count * 4)))
        vertices = []
        for pos, (color, uv, normal, tangent) in zip(positions, shading):
            rgba = tuple(((color >> (8 * i)) & 255) / 255 for i in range(4))
            tex_uv = struct.unpack("<ee", struct.pack("<I", uv))
            vertices.append(Vertex(pos, tex_uv, rgba, oct_decode(normal),
                                   oct_decode(tangent, True), 1 if tangent & 1 else -1))
        for first, length, base in surfaces:
            if first + length > len(indices) or length % 3:
                raise GeometryError("out-of-range or non-triangle surface")
            for index in indices[first:first + length]:
                if not 0 <= index + base < len(vertices):
                    raise GeometryError("surface baseVertex/index out of range")
        blobs.append(Blob(vertices, indices, surfaces))

    matrices = [None] * count
    visiting = set()

    def world(node):
        if not 0 <= node < count:
            raise GeometryError("instance parent/node out of range")
        if matrices[node] is not None:
            return matrices[node]
        if node in visiting:
            raise GeometryError("cyclic .geometry node hierarchy")
        visiting.add(node)
        parent = parents[node]
        matrices[node] = matmul(world(parent), locals_[node]) if parent >= 0 else locals_[node]
        visiting.remove(node)
        return matrices[node]

    triangles = []
    for node, geo, guid in instances:
        if geo >= len(blobs):
            raise GeometryError("instance geometry index out of range")
        matrix = world(node)
        blob = blobs[geo]
        transformed = [Vertex(transform(matrix, v.pos), v.uv, v.color,
                              transform_direction(matrix, v.normal),
                              transform_direction(matrix, v.tangent), v.handedness)
                       for v in blob.vertices]
        for first, length, base in blob.surfaces:
            for i in range(first, first + length, 3):
                a, b, c = (transformed[blob.indices[j] + base] for j in (i, i + 1, i + 2))
                triangles.append((a, b, c, guid))
    if not triangles:
        raise GeometryError(f"{path}: no captureable triangles")
    return triangles


def read_kv(path):
    rows = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            rows[key.strip()] = value.strip()
    return rows


def material_files(geometry: Path):
    mats = {}
    for path in sorted(geometry.parent.glob("*.material")):
        rows = read_kv(path)
        guid = rows.get("guid", "")
        if guid:
            if guid in mats:
                raise GeometryError(f"duplicate material GUID {guid} in {geometry.parent}")
            mats[guid] = (path, rows)
    return mats


def material_values(rows):
    def numbers(key, default):
        text = rows.get(key)
        if not text:
            return default
        values = tuple(float(part) for part in text.split())
        if len(values) != len(default):
            raise GeometryError(f"invalid material {key} value")
        return values

    return {
        "base": numbers("baseColor", (1, 1, 1, 1)),
        "metallic": float(rows.get("metallic", "1")),
        "roughness": float(rows.get("roughness", "1")),
        "emissive": numbers("emissive", (0, 0, 0)),
        "emissive_strength": float(rows.get("emissiveStrength", "1")),
        "occlusion_strength": float(rows.get("occlusionStrength", "1")),
        "normal_scale": float(rows.get("normalScale", "1")),
        "alpha_mode": rows.get("alphaMode", "opaque"),
        "alpha_cutoff": float(rows.get("alphaCutoff", "0.5")),
    }


def open_texture(material_path, ref, cache):
    if not ref:
        return None
    source = ref.removesuffix(" srgb").strip()
    path = material_path.parent / source
    wrap = "repeat"
    if path.suffix.lower() == ".texture":
        settings = read_kv(path)
        wrap = settings.get("wrap", "repeat")
        path = path.parent / settings.get("source", "")
    path = path.resolve()
    if path in cache:
        return cache[path], wrap
    if not path.is_file():
        raise GeometryError(f"missing material texture: {path}")
    if path.suffix.lower() == ".ktx2":
        raise GeometryError(f"KTX2 texture requires conversion to PNG before CPU capture: {path}")
    if Image is None:
        raise GeometryError("Pillow is required: python -m pip install pillow")
    with Image.open(path) as src:
        cache[path] = src.convert("RGBA")
    return cache[path], wrap


def texel(texture, uv):
    if texture is None:
        return (1.0, 1.0, 1.0, 1.0)
    image, wrap = texture

    def coord(value):
        if wrap == "clamp":
            return max(0, min(1 - 1e-9, value))
        if wrap == "mirror":
            value = value % 2
            return 2 - value if value > 1 else min(value, 1 - 1e-9)
        return value % 1

    u, v = coord(uv[0]), coord(uv[1])
    x = min(image.width - 1, int(u * image.width))
    y = min(image.height - 1, int((1 - v) * image.height))
    return tuple(c / 255 for c in image.getpixel((x, y)))


def clamp_byte(value):
    return max(0, min(255, round(value * 255)))


def material_pixels(material, vertex_color, uv, normal, tangent, handedness, ordinal):
    vals, textures = material
    base_tex = texel(textures.get("baseColor"), uv)
    rgba = tuple(vals["base"][i] * vertex_color[i] * base_tex[i] for i in range(4))
    if vals["alpha_mode"] == "opaque":
        rgba = rgba[:3] + (1.0,)
    elif vals["alpha_mode"] == "mask":
        if rgba[3] < vals["alpha_cutoff"]:
            return None
        rgba = rgba[:3] + (1.0,)
    elif rgba[3] <= 0:
        return None

    orm = texel(textures.get("metallicRoughness"), uv)
    occ = texel(textures.get("occlusion"), uv)
    roughness = vals["roughness"] * orm[1]
    metallic = vals["metallic"] * orm[2]
    occlusion = 1 - vals["occlusion_strength"] * (1 - occ[0])

    n = normalize(normal)
    if textures.get("normal") is not None:
        nm = texel(textures["normal"], uv)
        tn = normalize(((nm[0] * 2 - 1) * vals["normal_scale"],
                        (nm[1] * 2 - 1) * vals["normal_scale"], nm[2] * 2 - 1))
        t = normalize(tuple(tangent[i] - dot(tangent, n) * n[i] for i in range(3)))
        b = cross(n, t)
        n = normalize(tuple(tn[0] * t[i] + tn[1] * handedness * b[i] + tn[2] * n[i] for i in range(3)))

    em_tex = texel(textures.get("emissive"), uv)
    em = tuple(vals["emissive"][i] * vals["emissive_strength"] * em_tex[i] for i in range(3))
    alpha = clamp_byte(rgba[3])
    return {
        "albedo": tuple(clamp_byte(v) for v in rgba[:3]) + (alpha,),
        "normal": tuple(clamp_byte(v * .5 + .5) for v in n) + (alpha,),
        "orm": (clamp_byte(occlusion), clamp_byte(roughness), clamp_byte(metallic), alpha),
        "emissive": tuple(clamp_byte(v) for v in em) + (alpha,),
        "coverage": (255, 255, 255, alpha),
        "material_id": (ordinal & 255, (ordinal >> 8) & 255, (ordinal >> 16) & 255, alpha),
    }


def rasterize(triangles, materials, size):
    coords = [v.pos for a, b, c, _ in triangles for v in (a, b, c)]
    bounds_min = tuple(min(v[i] for v in coords) for i in range(3))
    bounds_max = tuple(max(v[i] for v in coords) for i in range(3))
    center = tuple((a + b) * .5 for a, b in zip(bounds_min, bounds_max))
    extent = tuple(b - a for a, b in zip(bounds_min, bounds_max))
    span = max(max(extent) * 1.05, 1e-5)
    captures = {}
    for view, right, up, front in VIEWS:
        layers = {ch: Image.new("RGBA", (size, size)) for ch in CHANNELS}
        pixels = {ch: im.load() for ch, im in layers.items()}
        depth = [-math.inf] * (size * size)
        for a, b, c, guid in triangles:
            points = []
            for v in (a, b, c):
                d = tuple(v.pos[i] - center[i] for i in range(3))
                points.append(((dot(d, right) / span + .5) * size,
                               (.5 - dot(d, up) / span) * size, dot(d, front)))
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = points
            def edge(ax, ay, bx, by, px, py):
                return (px - ax) * (by - ay) - (py - ay) * (bx - ax)
            area = edge(x0, y0, x1, y1, x2, y2)
            if abs(area) < 1e-9:
                continue
            min_x = max(0, math.floor(min(x0, x1, x2)))
            max_x = min(size - 1, math.ceil(max(x0, x1, x2)))
            min_y = max(0, math.floor(min(y0, y1, y2)))
            max_y = min(size - 1, math.ceil(max(y0, y1, y2)))
            material, ordinal = materials[guid]
            for y in range(min_y, max_y + 1):
                for x in range(min_x, max_x + 1):
                    px, py = x + .5, y + .5
                    w0 = edge(x1, y1, x2, y2, px, py) / area
                    w1 = edge(x2, y2, x0, y0, px, py) / area
                    w2 = 1 - w0 - w1
                    if min(w0, w1, w2) < -1e-7:
                        continue
                    z = w0 * z0 + w1 * z1 + w2 * z2
                    index = y * size + x
                    if z <= depth[index]:
                        continue
                    def blend(key):
                        return tuple(w0 * a.__dict__[key][i] +
                                     w1 * b.__dict__[key][i] +
                                     w2 * c.__dict__[key][i] for i in range(len(a.__dict__[key])))
                    # For captures the geometry is orthographic, so UV interpolation
                    # is affine (there is no camera perspective divide).
                    values = material_pixels(material, blend("color"), blend("uv"),
                                             blend("normal"), blend("tangent"),
                                             a.handedness, ordinal)
                    if values is None:
                        continue
                    depth[index] = z
                    for channel in CHANNELS:
                        pixels[channel][x, y] = values[channel]
        captures[view] = layers
    return captures, bounds_min, bounds_max


def geometry_identity(path):
    for manifest in sorted(path.parent.glob(path.stem + ".*.meta")):
        rows = read_kv(manifest)
        record = rows.get("geometry", rows.get("mesh", "")).split()
        if len(record) >= 2 and record[0] == path.name:
            return record[1]
    return ""


def prepare(path, size):
    triangles = load_geometry(path)
    sources = material_files(path)
    texture_cache = {}
    used = sorted({guid for *_, guid in triangles})
    materials = {}
    table = []
    for ordinal, guid in enumerate(used, start=1):
        if not guid:
            vals, textures, filename = material_values({}), {}, None
        else:
            if guid not in sources:
                raise GeometryError(f"{path}: material GUID {guid} has no .material beside the .geometry")
            mat_path, rows = sources[guid]
            vals = material_values(rows)
            textures = {slot: open_texture(mat_path, rows.get("tex." + slot, ""), texture_cache)
                        for slot in ("baseColor", "metallicRoughness", "normal", "occlusion", "emissive")}
            filename = mat_path.name
        materials[guid] = ((vals, textures), ordinal)
        table.append({"id": ordinal, "guid": guid, "file": filename})
    captures, aabb_min, aabb_max = rasterize(triangles, materials, size)
    return {
        "path": path,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "guid": geometry_identity(path),
        "aabb_min": aabb_min,
        "aabb_max": aabb_max,
        "triangles": len(triangles),
        "materials": table,
        "captures": captures,
    }


def next_power_two(value):
    return 1 << (max(1, value) - 1).bit_length()


def rect(x, y, width, height):
    return {"x": x, "y": y, "width": width, "height": height}


def paste_with_gutter(dst, image, x, y, gutter):
    w, h = image.size
    dst.paste(image, (x + gutter, y + gutter))
    # Replicate all four borders and the corners. This padding prevents
    # neighboring assets from leaking into a low-frequency lookup.
    dst.paste(image.crop((0, 0, 1, h)).resize((gutter, h)), (x, y + gutter))
    dst.paste(image.crop((w - 1, 0, w, h)).resize((gutter, h)), (x + gutter + w, y + gutter))
    dst.paste(image.crop((0, 0, w, 1)).resize((w, gutter)), (x + gutter, y))
    dst.paste(image.crop((0, h - 1, w, h)).resize((w, gutter)), (x + gutter, y + gutter + h))
    for sx, sy, tx, ty in ((0, 0, x, y), (w - 1, 0, x + gutter + w, y),
                           (0, h - 1, x, y + gutter + h),
                           (w - 1, h - 1, x + gutter + w, y + gutter + h)):
        dst.paste(Image.new("RGBA", (gutter, gutter), image.getpixel((sx, sy))), (tx, ty))


def write_atlas(assets, output, face_size, atlas_width, padding, levels):
    output.mkdir(parents=True, exist_ok=True)
    index = {
        "version": 1,
        "description": "CPU orthographic ±X/±Y/±Z captures; pixel rects, origin top-left",
        "channels": {
            "albedo": "RGBA8 sRGB base color times vertex color; A=coverage",
            "normal": "RGBA8 world-space XYZ mapped from [-1,1]; A=coverage",
            "orm": "RGBA8 R=occlusion G=roughness B=metallic A=coverage",
            "emissive": "RGBA8 emissive RGB; A=coverage",
            "coverage": "RGBA8 white RGB, A=visibility/alpha",
            "material_id": "RGBA8 24-bit little-endian local material ID, 0=background; A=coverage",
        },
        "views": [item[0] for item in VIEWS],
        "mips": [],
        "assets": [
            {"source": asset["path"].as_posix(), "sha256": asset["sha256"],
             "geometry_guid": asset["guid"], "triangle_count": asset["triangles"],
             "bounds": {"min": asset["aabb_min"], "max": asset["aabb_max"],
                        "size": [b - a for a, b in zip(asset["aabb_min"], asset["aabb_max"])]},
             "materials": asset["materials"], "mips": []}
            for asset in assets
        ],
    }
    resample = Image.Resampling if hasattr(Image, "Resampling") else Image
    for level in range(levels):
        size = max(1, face_size >> level)
        gutter = max(1, padding >> level)
        stride = size + 2 * gutter
        block_width, block_height = 3 * stride, 2 * stride
        width = next_power_two(max(atlas_width >> level, block_width))
        cols = max(1, width // block_width)
        height = next_power_two(((len(assets) + cols - 1) // cols) * block_height)
        output_images = {ch: Image.new("RGBA", (width, height)) for ch in CHANNELS}
        files = {ch: f"{ch}_mip{level}.png" for ch in CHANNELS}
        index["mips"].append({"level": level, "width": width, "height": height,
                              "face_size": size, "padding": gutter, "files": files})
        for i, asset in enumerate(assets):
            x, y = i % cols * block_width, i // cols * block_height
            faces = {}
            for view_i, (view, _, _, _) in enumerate(VIEWS):
                vx, vy = x + view_i % 3 * stride, y + view_i // 3 * stride
                faces[view] = rect(vx + gutter, vy + gutter, size, size)
                for ch in CHANNELS:
                    base = asset["captures"][view][ch]
                    if level == 0:
                        tile = base
                    else:
                        filter_ = resample.NEAREST if ch == "material_id" else resample.BOX
                        tile = base.resize((size, size), resample=filter_)
                    paste_with_gutter(output_images[ch], tile, vx, vy, gutter)
            index["assets"][i]["mips"].append({
                "level": level, "rect": rect(x, y, block_width, block_height),
                "views": faces,
            })
        for ch, image in output_images.items():
            image.save(output / files[ch])
    (output / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    return index


def collect_inputs(paths):
    found = set()
    for path in paths:
        if path.is_file() and path.suffix.lower() == ".geometry":
            found.add(path.resolve())
        elif path.is_dir():
            found.update(p.resolve() for p in path.rglob("*.geometry") if p.is_file())
        else:
            raise GeometryError(f"not a .geometry file or directory: {path}")
    if not found:
        raise GeometryError("no .geometry files found")
    return sorted(found, key=lambda p: p.as_posix())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help=".geometry files or recursively scanned folders")
    parser.add_argument("--output", "-o", type=Path, required=True, help="output directory (PNG mips + index.json)")
    parser.add_argument("--face-size", type=int, default=128, help="pixels per signed-axis view at mip 0 (default: 128)")
    parser.add_argument("--atlas-width", type=int, default=2048, help="target mip-0 atlas width (default: 2048)")
    parser.add_argument("--padding", type=int, default=8, help="edge-extended gutter pixels (default: 8)")
    parser.add_argument("--mips", type=int, default=0, help="mip count (0 = down to 1x1)")
    args = parser.parse_args(argv)
    if Image is None:
        parser.error("Pillow is required: python -m pip install pillow")
    if not 4 <= args.face_size <= 2048 or args.face_size & (args.face_size - 1):
        parser.error("--face-size must be a power of two in [4, 2048]")
    if args.padding < 1 or args.atlas_width < 1 or args.mips < 0:
        parser.error("--padding and --atlas-width must be positive; --mips cannot be negative")
    max_levels = args.face_size.bit_length()
    if args.mips > max_levels:
        parser.error("--mips exceeds the full mip chain")
    try:
        paths = collect_inputs(args.inputs)
        assets = [prepare(path, args.face_size) for path in paths]
        index = write_atlas(assets, args.output, args.face_size, args.atlas_width,
                            args.padding, args.mips or max_levels)
    except (OSError, GeometryError, ValueError, struct.error) as exc:
        print(f"geometry_capture_atlas: {exc}", file=sys.stderr)
        return 2
    print(f"Captured {len(index['assets'])} .geometry assets into {args.output} "
          f"({len(index['mips'])} mip levels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
