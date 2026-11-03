#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Generate deterministic glTF/.tour material evaluation fixtures, stdlib only.

The Cornell-style enclosure is a practical diagnostic scene, not a
path-traced or physically calibrated Cornell-box reference.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

MATERIALS = [
    ("dielectric_rough", [0.8, 0.8, 0.8, 1], 0.0, 0.85),
    ("dielectric_smooth", [0.8, 0.8, 0.8, 1], 0.0, 0.12),
    ("black_plastic", [0.04, 0.04, 0.04, 1], 0.0, 0.42),
    ("metal_rough", [0.8, 0.82, 0.86, 1], 1.0, 0.85),
    ("metal_smooth", [0.8, 0.82, 0.86, 1], 1.0, 0.12),
    ("copper_brushed", [0.92, 0.48, 0.22, 1], 1.0, 0.48),
    ("mixed_metal", [0.7, 0.65, 0.5, 1], 0.5, 0.25),
    ("roughness_clamp", [0.78, 0.2, 0.1, 1], 0.0, 0.01),
    ("white_warning", [0.98, 0.98, 0.98, 1], 0.0, 0.6),
    ("toon_cel", [0.27, 0.57, 0.92, 1], 0.0, 0.85),
]
# Fixed positions referenced by material_lab/suite.json camera bookmarks.
POSITIONS = [(float(col - 1) * 2.15, 4.7 - float(row) * 1.8, 0.0)
             for row in range(3) for col in range(3)]
# Leave original bookmarks intact; extra sphere stands behind the grid.
POSITIONS.append((-3.2, 4.7, -2.15))

ENVIRONMENT_SETTINGS = {
    "studio": {"sunIntensity": "6500", "sunAzimuth": "25", "sunElevation": "47",
               "skyColor": "0.6 0.65 0.72", "skyIntensity": "550"},
    "cornell": {"sunIntensity": "0", "sunAzimuth": "25", "sunElevation": "47",
                "skyColor": "0.25 0.25 0.25", "skyIntensity": "100"},
    "outdoor": {"sunIntensity": "11000", "sunAzimuth": "45", "sunElevation": "62",
                "skyColor": "0.27 0.45 0.85", "skyIntensity": "700"},
}


def sphere(slices=24, stacks=16):
    positions, normals, indices = [], [], []
    for j in range(stacks + 1):
        phi = math.pi * j / stacks
        for i in range(slices + 1):
            theta = 2.0 * math.pi * i / slices
            n = (math.sin(phi) * math.cos(theta), math.cos(phi),
                 math.sin(phi) * math.sin(theta))
            normals.append(n)
            positions.append(tuple(0.67 * a for a in n))
    for j in range(stacks):
        for i in range(slices):
            a = j * (slices + 1) + i
            b = a + 1
            c = a + slices + 1
            d = c + 1
            indices.extend((a, b, c, b, d, c))
    return positions, normals, indices


def quad(a, b, c, d, normal):
    return [a, b, c, d], [normal] * 4, [0, 1, 2, 0, 2, 3]


class Gltf:
    def __init__(self):
        self.data = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "Tourmaline material_lab_fixtures"},
                    "scene": 0, "scenes": [{"nodes": []}], "nodes": [],
                    "meshes": [], "materials": [], "buffers": [],
                    "bufferViews": [], "accessors": []}
        self.ref_meshes = []
        self.ref_positions, self.ref_normals = [], []
        self.ref_indices, self.ref_tri_materials = [], []

    def accessor(self, values, fmt, kind, target):
        align = 4 if fmt == "f" else 2
        while len(self.data) % align:
            self.data.append(0)
        off = len(self.data)
        flat = [v for row in values for v in
                (row if isinstance(row, (list, tuple)) else (row,))]
        self.data.extend(struct.pack("<" + fmt * len(flat), *flat))
        bv = len(self.doc["bufferViews"])
        self.doc["bufferViews"].append({"buffer": 0, "byteOffset": off,
                                       "byteLength": len(self.data) - off, "target": target})
        index = len(self.doc["accessors"])
        acc = {"bufferView": bv, "componentType": 5126 if fmt == "f" else 5123,
               "count": len(values), "type": kind}
        if kind == "VEC3" and fmt == "f":
            acc["min"] = [min(v[i] for v in values) for i in range(3)]
            acc["max"] = [max(v[i] for v in values) for i in range(3)]
        self.doc["accessors"].append(acc)
        return index

    def mesh(self, name, geometry, material):
        positions, normals, indices = geometry
        pos = self.accessor(positions, "f", "VEC3", 34962)
        norm = self.accessor(normals, "f", "VEC3", 34962)
        ind = self.accessor(indices, "H", "SCALAR", 34963)
        mi = len(self.doc["meshes"])
        self.ref_meshes.append((positions, normals, indices, material))
        self.doc["meshes"].append({"name": name, "primitives": [
            {"attributes": {"POSITION": pos, "NORMAL": norm},
             "indices": ind, "material": material}]})
        return mi

    def node(self, name, mesh, translation=None):
        node = {"name": name, "mesh": mesh}
        if translation is not None:
            node["translation"] = translation
        idx = len(self.doc["nodes"])
        self.doc["nodes"].append(node)
        self.doc["scenes"][0]["nodes"].append(idx)
        positions, normals, indices, material = self.ref_meshes[mesh]
        offset = len(self.ref_positions)
        t = translation or (0, 0, 0)
        self.ref_positions.extend([[p[i] + t[i] for i in range(3)] for p in positions])
        self.ref_normals.extend(normals)
        self.ref_indices.extend(offset + index for index in indices)
        self.ref_tri_materials.extend([material] * (len(indices) // 3))

    def material(self, name, color, metal=0.0, rough=1.0):
        idx = len(self.doc["materials"])
        self.doc["materials"].append({"name": name, "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorFactor": color,
                                     "metallicFactor": metal,
                                     "roughnessFactor": rough}})
        return idx

    def save(self, path, environment):
        self.doc["buffers"] = [{"uri": path.with_suffix(".bin").name,
                                "byteLength": len(self.data)}]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.doc, indent=2) + "\n", encoding="utf-8")
        path.with_suffix(".bin").write_bytes(self.data)

        # The CPU reference consumes the SAME authored mesh/node geometry
        # used for the Vulkan scene, without relying on a cooked GPU pak.
        env = ENVIRONMENT_SETTINGS[environment]
        az, el = math.radians(float(env["sunAzimuth"])), math.radians(float(env["sunElevation"]))
        cos_el = math.cos(el)
        sky = [float(v) for v in env["skyColor"].split()]
        spots = []
        if environment == "cornell":
            spots = [{"position": [0, 5.5, 1.2], "direction": [0, -1, 0],
                      "color": [3000, 2880, 2640], "range": 15.0,
                      "cosInner": -1.0, "cosOuter": -1.0}]
        ref = {
            "schema_version": 1, "positions": self.ref_positions,
            "normals": self.ref_normals, "indices": self.ref_indices,
            "triangle_materials": self.ref_tri_materials,
            "materials": [{"baseColor": m["pbrMetallicRoughness"]["baseColorFactor"][:3],
                           "metallic": m["pbrMetallicRoughness"]["metallicFactor"],
                           "roughness": m["pbrMetallicRoughness"]["roughnessFactor"]}
                          for m in self.doc["materials"]],
            "environment": {
                "sunDirection": [math.sin(az) * cos_el, math.sin(el), math.cos(az) * cos_el],
                "sunColor": [1.0, 0.95, 0.90],
                "sunIntensity": float(env["sunIntensity"]),
                "skyZenith": sky, "skyHorizon": sky,
                "skyGround": [0.5 * v for v in sky],
                "skyIntensity": float(env["skyIntensity"]),
                "skyExponent": 2.0, "spots": spots,
            },
        }
        path.with_suffix(".refscene.json").write_text(
            json.dumps(ref, separators=(",", ":")) + "\n", encoding="utf-8")


def make_gltf(path, environment):
    gl = Gltf()
    ball = sphere()
    for (name, color, metal, rough), center in zip(MATERIALS, POSITIONS):
        material = gl.material(name, color, metal, rough)
        gl.node(name, gl.mesh(name, ball, material), list(center))
    floor = gl.material("floor_gray", [0.42, 0.42, 0.42, 1])
    gl.node("floor", gl.mesh("floor", quad(
        (-4, 0, -3.5), (4, 0, -3.5), (4, 0, 4), (-4, 0, 4), (0, 1, 0)), floor))
    if environment == "cornell":
        walls = [
            ("left_red", quad((-4, 0, -3.5), (-4, 0, 4),
                              (-4, 6.3, 4), (-4, 6.3, -3.5), (1, 0, 0)),
             [0.65, 0.05, 0.05, 1]),
            ("right_green", quad((4, 0, 4), (4, 0, -3.5),
                                 (4, 6.3, -3.5), (4, 6.3, 4), (-1, 0, 0)),
             [0.08, 0.5, 0.09, 1]),
            ("back_white", quad((4, 0, -3.5), (-4, 0, -3.5),
                                (-4, 6.3, -3.5), (4, 6.3, -3.5), (0, 0, 1)),
             [0.82, 0.82, 0.82, 1]),
            ("ceiling_white", quad((-4, 6.3, -3.5), (-4, 6.3, 4),
                                   (4, 6.3, 4), (4, 6.3, -3.5), (0, -1, 0)),
             [0.82, 0.82, 0.82, 1]),
        ]
        for name, geom, color in walls:
            material = gl.material(name, color)
            gl.node(name, gl.mesh(name, geom, material))
    gl.save(path, environment)


def make_tour(path, environment):
    settings = ENVIRONMENT_SETTINGS
    lines = [
        f"//Name: {environment}.tour",
        f'reference -root -n 0 -source "content/models/{environment}/{environment}.geometry" -name "ROOT";',
        'fileInfo -k "application" -v "tourmaline-tour";',
        'fileInfo -k "product" -v "Tourmaline";',
        'fileInfo -k "version" -v "1";',
    ]
    if environment == "cornell":
        lines += [
            'createLight -p 0 -name "CornellKey" -at point -t 0 5.5 1.2 3000 15 0 0 0 0;',
            '\tsetLightAttr -ref 0 -at color -type "double3" -t 1 0.96 0.88;',
        ]
    for key, value in settings[environment].items():
        lines.append(f'editorSet -k "env.default.{key}" -v "{value}";')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate(project):
    project.mkdir(parents=True, exist_ok=True)
    (project / "project.tourproj").write_text(
        "name=MaterialLab\nguid=08a662b4a11a46ca865b899efb0ef202\nversion=2\n",
        encoding="utf-8")
    # The legacy single-scene build lane reads this file and would
    # otherwise run Cook's --cubemap capture stage. These fixtures are
    # ANALYTIC-SKY environments by design: the auto probe lands inside
    # the centre sphere (bounds centre + 1.5 m), bakes a black capture
    # and the pak gate refuses the empty manifest. A baked single-probe
    # cubemap of a sphere grid is also the wrong lighting reference for
    # these suites — the renderer's analytic sky is the intended one.
    settings = project / ".tourmaline"
    settings.mkdir(parents=True, exist_ok=True)
    (settings / "build-settings.txt").write_text(
        "# material_lab: analytic-sky environments; no baked captures\n"
        "cubemap=off\n",
        encoding="utf-8")
    for env in ("studio", "cornell", "outdoor"):
        make_gltf(project / "content" / "models" / env / f"{env}.gltf", env)
        make_tour(project / "content" / "scenes" / f"{env}.tour", env)
    return project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path,
                        help="output Project source root (not a cooked pak)")
    args = parser.parse_args()
    print(f"Generated source fixtures: {generate(args.project.resolve())}")


if __name__ == "__main__":
    main()
