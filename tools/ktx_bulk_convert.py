#!/usr/bin/env python3
# ktx_bulk_convert.py
# Usage:
#   python ktx_bulk_convert.py <tools_dir> <input_dir> <output_dir> [--mode etc1s|uastc] [--threads N] [--no-mips]
#        [--lower-left] [--patterns ".png;.jpg;.jpeg;.tga;.bmp;.tif;.tiff;.hdr;.exr"] [--dry-run]
#
# CMake example:
# add_custom_command(
#   TARGET YourTarget POST_BUILD
#   COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ktx_bulk_convert.py
#           ${CMAKE_SOURCE_DIR}/third_party/ktx/bin
#           ${CMAKE_SOURCE_DIR}/assets/textures
#           ${CMAKE_BINARY_DIR}/assets_ktx2
#           --mode uastc --threads 0
#   VERBATIM
# )
#
# Behavior:
# - Detects color maps (sRGB), linear scalar maps (Rough/Metal/AO/etc), and normals (linear + normal_mode).
# - Preserves folder structure under the output dir.
# - Generates mipmaps by default (disable with --no-mips).
# - Exits non-zero if any conversion fails.

import argparse
import os
import sys
import re
import shutil
import subprocess
from pathlib import Path

COLOR_HINTS = re.compile(r"(albedo|basecolor|base_color|base-colou?r|color|diffuse|albd|emissive|emission|al)", re.I)
LINEAR_HINTS = re.compile(r"(rough|metal|ao|height|mask|spec|gloss|rma|orm|opacity|trans|disp|displ|roughness|metalness|metallic|ambientocclusion|occlusion)", re.I)
NORMAL_HINTS = re.compile(r"(normal|norm|_n\b|_n\.)", re.I)

DEFAULT_PATTERNS = ".png;.jpg;.jpeg;.tga;.bmp;.tif;.tiff;.hdr;.exr"
import json


def build_texture_classification(gltf_path: Path):
    with open(gltf_path, "r", encoding="utf-8") as f:
        gltf = json.load(f)

    textures = gltf.get("textures", [])
    images = gltf.get("images", [])

    classification = {}

    def mark(texture_index, kind):
        if texture_index is None:
            return
        if texture_index < 0 or texture_index >= len(textures):
            return

        source = textures[texture_index].get("source")
        if source is None or source < 0 or source >= len(images):
            return

        uri = images[source].get("uri")
        if not uri or uri.startswith("data:"):
            return

        classification[Path(uri).as_posix()] = kind

    for mat in gltf.get("materials", []):
        pbr = mat.get("pbrMetallicRoughness", {})

        tex = pbr.get("baseColorTexture")
        if tex:
            mark(tex.get("index"), "srgb")

        tex = pbr.get("metallicRoughnessTexture")
        if tex:
            mark(tex.get("index"), "linear")

        tex = mat.get("normalTexture")
        if tex:
            mark(tex.get("index"), "normal")

        tex = mat.get("occlusionTexture")
        if tex:
            mark(tex.get("index"), "linear")

        tex = mat.get("emissiveTexture")
        if tex:
            mark(tex.get("index"), "srgb")

    return classification

classification =build_texture_classification(r"F:\REPOS\vulkantoy\app\content\models\sponza\sponza.gltf")


def find_toktx(tools_dir: Path) -> Path:
    exe = "toktx.exe" if os.name == "nt" else "toktx"
    candidate = tools_dir / exe
    if candidate.exists():
        return candidate.resolve()
    # Also try tools_dir/bin
    candidate = tools_dir / "bin" / exe
    if candidate.exists():
        return candidate.resolve()
    # Fallback to PATH
    found = shutil.which(exe)
    if found:
        return Path(found).resolve()
    sys.stderr.write(f"[ktx] ERROR: Could not find '{exe}' in '{tools_dir}' or PATH.\n")
    sys.exit(2)

def classify_texture(name: str):
    """
    Returns one of: 'normal', 'linear', 'srgb'
    """
    base = os.path.basename(name)
    if NORMAL_HINTS.search(base):
        return "normal"
    if LINEAR_HINTS.search(base):
        return "linear"
    if COLOR_HINTS.search(base):
        return "srgb"
    # Heuristic fallback:
    # If filename hints at color-like things use sRGB, otherwise linear.
    return "srgb"

def build_cmd(toktx: Path, mode: str, kind: str, src: Path, dst: Path, genmips: bool, threads: int, lower_left: bool):
    # Shared flags
    cmd = [str(toktx), "--t2"]
    if genmips:
        cmd += ["--genmipmap"]
    if threads is not None:
        cmd += ["--threads", str(threads)]

    if lower_left:
        cmd += ["--lower_left_maps_to_s0t0"]

    # Encoding mode
    if mode == "etc1s":
        # Smaller files, distribution-friendly
        cmd += ["--encode", "etc1s", "--clevel", "4", "--qlevel", "128"]
    elif mode == "uastc":
        # Higher quality + zstd supercompression
        cmd += ["--encode", "uastc", "--uastc_quality", "2", "--uastc_rdo_l", "1.0", "--zcmp", "18"]
    else:
        raise ValueError(f"Unknown mode: {mode}")

    # Colorimetry / data handling
    if kind == "normal":
        cmd += ["--assign_oetf", "linear", "--assign_primaries", "none", "--normal_mode", "--input_swizzle", "rgb1"]
    elif kind == "linear":
        cmd += ["--assign_oetf", "linear", "--assign_primaries", "none"]
    else:  # srgb
        cmd += ["--assign_oetf", "srgb"]

    # Output path then input
    cmd += [str(dst), str(src)]
    return cmd

def should_take(path: Path, patterns):
    ext = path.suffix.lower()
    return any(ext == p.strip().lower() for p in patterns)

def main():
    parser = argparse.ArgumentParser(description="Bulk convert textures to KTX2 via toktx.")
    parser.add_argument("tools_dir", type=Path, help="Directory containing toktx (or a bin/ subfolder with it).")
    parser.add_argument("input_dir", type=Path, help="Input folder to scan for textures.")
    parser.add_argument("output_dir", type=Path, help="Output folder for .ktx2 files (mirrors structure).")
    parser.add_argument("--mode", choices=["etc1s", "uastc"], default="etc1s", help="Encoding mode. Default: etc1s")
    parser.add_argument("--threads", type=int, default=0, help="Threads for toktx (0 = auto).")
    parser.add_argument("--no-mips", action="store_true", help="Disable mipmap generation.")
    parser.add_argument("--lower-left", action="store_true", help="Map lower-left to (s=0,t=0) for GL-style origin.")
    parser.add_argument("--patterns", default=DEFAULT_PATTERNS, help=f"Semicolon-separated extensions to include. Default: {DEFAULT_PATTERNS}")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing.")
    args = parser.parse_args()

    toktx = find_toktx(args.tools_dir)
    in_root = args.input_dir.resolve()
    out_root = args.output_dir.resolve()
    patterns = [("." + p.lstrip(".")).lower() for p in args.patterns.split(";") if p.strip()]
    genmips = not args.no_mips

    if not in_root.exists():
        sys.stderr.write(f"[ktx] ERROR: Input dir not found: {in_root}\n")
        sys.exit(2)

    out_root.mkdir(parents=True, exist_ok=True)

    total = 0
    converted = 0
    skipped = 0
    failed = 0
    commands_run = 0

    for src in in_root.rglob("*"):
        if not src.is_file():
            continue
        if not should_take(src, patterns):
            continue
        total += 1

        rel = src.relative_to(in_root)
        dst = (out_root / rel).with_suffix(".ktx2")
        dst.parent.mkdir(parents=True, exist_ok=True)

        kind = classification.get(rel)
        if kind == "normal":
            cmd += [
                "--assign_oetf", "linear",
                "--assign_primaries", "none",
            ]
        if kind is None:
            print(f"[ktx] WARNING: Texture not referenced by glTF: {rel}")
            kind = "srgb"

        cmd = build_cmd(
            toktx=toktx,
            mode=args.mode,
            kind=kind,
            src=src,
            dst=dst,
            genmips=genmips,
            threads=args.threads,
            lower_left=args.lower_left
        )

        # Skip if up-to-date (simple timestamp check)
        try:
            if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
                print(f"[ktx] SKIP (up-to-date): {rel}")
                skipped += 1
                continue
        except Exception:
            pass

        print("[ktx] CMD:", " ".join(f'"{c}"' if " " in c else c for c in cmd))
        commands_run += 1
        if args.dry_run:
            converted += 1
            continue

        try:
            subprocess.run(cmd, check=True)
            converted += 1
        except subprocess.CalledProcessError as e:
            failed += 1
            # remove partial result to avoid poisoning subsequent up-to-date checks
            try:
                if dst.exists():
                    dst.unlink()
            except Exception:
                pass
            sys.stderr.write(f"[ktx] ERROR ({rel}): exit code {e.returncode}\n")

    print(f"[ktx] Summary: total={total} converted={converted} skipped={skipped} failed={failed} ran={commands_run} mode={args.mode} mips={'on' if genmips else 'off'}")
    sys.exit(1 if failed > 0 else 0)

if __name__ == "__main__":
    main()
