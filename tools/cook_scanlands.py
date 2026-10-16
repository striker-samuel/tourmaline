#!/usr/bin/env python3
# cook_scanlands.py — stage + cook the Scanlands GLB scene (no GI bake).
#
# The source is a binary .glb (F:/content/models/scanlands.glb) with all
# textures + geometry embedded (bufferViews, no external URIs). The runtime /
# PakCooker pipeline only accepts loose .gltf + .bin + .ktx2, so this script:
#   1. Parses the GLB container (JSON chunk + BIN chunk),
#   2. Extracts embedded images to loose files (mimeType -> .jpg/.png),
#   3. Encodes each to .ktx2 with toktx (uastc + zstd, glTF-driven
#      srgb/linear/normal classification, same flags as tools/cook_bistro.py),
#   4. Stages models/Scanlands/<textures> + a rewritten scanlands.gltf whose
#      image URIs point at the .ktx2 files (buffers[0].uri = scanlands.bin),
#   5. Runs PakCooker --meshlets on --lods on to produce content/paks/scanlands.pak.
#
# There is intentionally NO GiBaker step: scanlands runs GI-less (analytic
# sky IBL ambient; the app falls back to SkyIbl when content/gi/scanlands.sggi
# is absent). Bake one later with:
#   GiBaker --content-dir F:/content --in-vfs models/Scanlands/scanlands.gltf
#           --out F:/content/gi/scanlands.sggi
#
# Usage (PowerShell, from repo root F:/REPOS/tourmaline):
#   py tools/cook_scanlands.py [--glb F:/content/models/scanlands.glb]
#       [--app-content F:/content] [--workers 8] [--skip-convert] [--skip-pak]
#
# --skip-convert reuses already-staged .ktx2 files. --skip-pak stops after
# staging (lets you run PakCooker manually).

import argparse
import json
import re
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOKTX = Path(r"C:\Program Files\KTX-Software\bin\toktx.exe")
DEFAULT_GLB = Path(r"F:/content/models/scanlands.glb")


def classify_images(gltf):
    """Map image index -> 'srgb' | 'linear' | 'normal' from material usage.

    Mirrors tools/cook_bistro.py (incl. KHR_materials_pbrSpecularGlossiness).
    First writer wins — in practice an image serves one purpose.
    """
    kind = {}

    def mark(tex_index, k):
        if tex_index is None:
            return
        textures = gltf.get("textures", [])
        images = gltf.get("images", [])
        if tex_index < 0 or tex_index >= len(textures):
            return
        tex = textures[tex_index]
        src = tex.get("source", -1)
        if src is not None and 0 <= src < len(images):
            kind.setdefault(src, k)

    for mat in gltf.get("materials", []):
        pbr = mat.get("pbrMetallicRoughness", {})
        if isinstance(pbr.get("baseColorTexture"), dict):
            mark(pbr["baseColorTexture"].get("index"), "srgb")
        if isinstance(pbr.get("metallicRoughnessTexture"), dict):
            mark(pbr["metallicRoughnessTexture"].get("index"), "linear")
        if isinstance(mat.get("normalTexture"), dict):
            mark(mat["normalTexture"].get("index"), "normal")
        if isinstance(mat.get("occlusionTexture"), dict):
            mark(mat["occlusionTexture"].get("index"), "linear")
        if isinstance(mat.get("emissiveTexture"), dict):
            mark(mat["emissiveTexture"].get("index"), "srgb")
        sg = mat.get("extensions", {}).get(
            "KHR_materials_pbrSpecularGlossiness", {})
        if isinstance(sg.get("diffuseTexture"), dict):
            mark(sg["diffuseTexture"].get("index"), "srgb")
        if isinstance(sg.get("specularGlossinessTexture"), dict):
            mark(sg["specularGlossinessTexture"].get("index"), "linear")
    return kind


def toktx_cmd(toktx, kind, src_img, dst_ktx, uastc_quality, zcmp):
    cmd = [str(toktx), "--t2", "--genmipmap", "--threads", "2",
           "--encode", "uastc", "--uastc_quality", str(uastc_quality),
           "--uastc_rdo_l", "1.0", "--zcmp", str(zcmp)]
    if kind == "normal":
        cmd += ["--assign_oetf", "linear", "--assign_primaries", "none",
                "--normal_mode"]
    elif kind == "linear":
        cmd += ["--assign_oetf", "linear", "--assign_primaries", "none"]
    else:
        cmd += ["--assign_oetf", "srgb"]
    cmd += [str(dst_ktx), str(src_img)]
    return cmd


def sanitize(name):
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", name or "")
    return name.strip("._") or "image"


def parse_glb(path):
    """Returns (gltf_dict, bin_chunk_bytes). Raises on bad magic."""
    data = Path(path).read_bytes()
    if len(data) < 12 or data[0:4] != b"glTF":
        raise ValueError(f"not a GLB file: {path}")
    # ver = struct.unpack("<I", data[4:8])[0]  # 2 expected; accept any
    json_len = struct.unpack("<I", data[12:16])[0]
    json_type = data[16:20]
    if json_type != b"JSON":
        raise ValueError("GLB missing JSON chunk")
    gltf = json.loads(data[20:20 + json_len].decode("utf-8"))
    off = 20 + json_len
    bin_chunk = b""
    if off + 8 <= len(data):
        bin_len = struct.unpack("<I", data[off:off + 4])[0]
        bin_type = data[off + 4:off + 8]
        if bin_type == b"BIN\x00":
            bin_chunk = data[off + 8:off + 8 + bin_len]
    return gltf, bin_chunk


def main():
    ap = argparse.ArgumentParser(description="Stage + cook Scanlands GLB (no GI).")
    ap.add_argument("--glb", default=str(DEFAULT_GLB))
    ap.add_argument("--app-content", default=r"F:/content")
    ap.add_argument("--scene-dir-name", default="Scanlands")
    ap.add_argument("--gltf-name", default="scanlands.gltf")
    ap.add_argument("--bin-name", default="scanlands.bin")
    ap.add_argument("--pak-name", default="scanlands.pak")
    ap.add_argument("--toktx", default=str(DEFAULT_TOKTX))
    ap.add_argument("--pakcooker", default=str(REPO_ROOT / "build" / "Release" / "PakCooker.exe"))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--uastc-quality", type=int, default=1)
    ap.add_argument("--zcmp", type=int, default=12)
    ap.add_argument("--skip-convert", action="store_true")
    ap.add_argument("--skip-pak", action="store_true")
    ap.add_argument("--meshlets", default="on")
    ap.add_argument("--lods", default="on")
    ap.add_argument("--lod-ratios", default="0.6,0.3,0.12")
    ap.add_argument("--lod-error", type=float, default=0.02)
    args = ap.parse_args()

    glb_path = Path(args.glb)
    content = Path(args.app_content)
    stage = content / "models" / args.scene_dir_name
    if not glb_path.exists():
        sys.stderr.write(f"ERROR: glb not found: {glb_path}\n")
        return 2
    toktx = Path(args.toktx)
    if not toktx.exists():
        sys.stderr.write(f"ERROR: toktx not found: {toktx}\n")
        return 2

    print(f"[scanlands] parsing {glb_path} ({glb_path.stat().st_size / 1e6:.1f} MB)...")
    gltf, bin_chunk = parse_glb(glb_path)
    images = gltf.get("images", [])
    buffers = gltf.get("buffers", [])
    views = gltf.get("bufferViews", [])
    kinds = classify_images(gltf)
    print(f"[scanlands] nodes={len(gltf.get('nodes', []))} meshes={len(gltf.get('meshes', []))} "
          f"materials={len(gltf.get('materials', []))} images={len(images)} "
          f"classified={len(kinds)} bin={len(bin_chunk) / 1e6:.1f} MB")

    stage.mkdir(parents=True, exist_ok=True)
    tex_dir = stage / "textures"
    tex_dir.mkdir(parents=True, exist_ok=True)

    # 1. Write BIN chunk (GLB buffer 0 has no uri; externalize it).
    if not buffers:
        sys.stderr.write("ERROR: glb has no buffers\n")
        return 1
    bin_path = stage / args.bin_name
    if not args.skip_convert or not bin_path.exists():
        bin_path.write_bytes(bin_chunk)
        print(f"[scanlands] wrote {bin_path} ({len(bin_chunk) / 1e6:.1f} MB)")
    buffers[0]["uri"] = args.bin_name
    buffers[0]["byteLength"] = len(bin_chunk)

    # 2. Extract embedded images to loose files.
    MIME_EXT = {"image/jpeg": ".jpg", "image/jpg": ".jpg",
                "image/png": ".png", "image/webp": ".webp",
                "image/bmp": ".bmp"}
    jobs = []  # (kind, src_img, dst_ktx)
    for idx, img in enumerate(images):
        uri = img.get("uri", "")
        if uri and not uri.startswith("data:"):
            print(f"[scanlands] image {idx} already external ({uri}), leaving as-is")
            continue
        if "bufferView" not in img:
            print(f"[scanlands] WARNING: image {idx} has no uri nor bufferView, skipped")
            continue
        bv = views[img["bufferView"]]
        off = bv.get("byteOffset", 0)
        length = bv["byteLength"]
        payload = bin_chunk[off:off + length]
        mime = img.get("mimeType", "image/png")
        ext = MIME_EXT.get(mime.lower(), ".png")
        base = sanitize(img.get("name", f"image_{idx}"))
        if not base.lower().endswith(ext):
            base += ext
        # Prefix with index for stable unique names.
        src_img = tex_dir / f"{idx:02d}_{base}"
        if not src_img.exists() or src_img.stat().st_size != len(payload):
            src_img.write_bytes(payload)
        kind = kinds.get(idx, "srgb")
        dst_ktx = src_img.with_suffix(".ktx2")
        jobs.append((kind, src_img, dst_ktx))
        # Rewrite the gltf image entry to the staged ktx2 (POSIX uri).
        rel = dst_ktx.relative_to(stage).as_posix()
        img["uri"] = rel
        img["mimeType"] = "image/ktx2"
        img.pop("bufferView", None)
        img.pop("byteLength", None)

    print(f"[scanlands] embedded images extracted: {len(jobs)}")

    # 3. Convert to ktx2 (skip up-to-date unless --skip-convert reuses all).
    if not args.skip_convert:
        t0 = time.time()
        ok = fail = skipped = 0

        def convert_one(job):
            kind, src_img, dst_ktx = job
            try:
                if dst_ktx.exists() and dst_ktx.stat().st_mtime >= src_img.stat().st_mtime:
                    return (src_img.name, True, "skip-up-to-date")
                cmd = toktx_cmd(toktx, kind, src_img, dst_ktx,
                                args.uastc_quality, args.zcmp)
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
                if r.returncode != 0:
                    return (src_img.name, False, f"toktx rc={r.returncode}: {r.stderr[-2000:]}")
                return (src_img.name, True, "ok")
            except Exception as e:  # noqa: BLE001
                return (src_img.name, False, f"{type(e).__name__}: {e}")

        jobs_sorted = sorted(jobs, key=lambda j: j[1].name)
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futs = {pool.submit(convert_one, j): j[1].name for j in jobs_sorted}
            done = 0
            for fut in as_completed(futs):
                name, good, msg = fut.result()
                done += 1
                if good:
                    ok += 1
                    if msg == "skip-up-to-date":
                        skipped += 1
                else:
                    fail += 1
                    print(f"[scanlands] FAILED {name}: {msg}")
                if done % 10 == 0 or done == len(jobs_sorted):
                    print(f"[scanlands] convert {done}/{len(jobs_sorted)} "
                          f"(ok={ok} fail={fail}) {time.time() - t0:.0f}s")
        print(f"[scanlands] convert done: ok={ok} (skipped={skipped}) "
              f"fail={fail} in {time.time() - t0:.0f}s")
        if fail:
            sys.stderr.write(f"[scanlands] {fail} conversions failed\n")
            return 1
    else:
        print("[scanlands] --skip-convert: reusing staged ktx2 files")
        # Still verify every image has its ktx2 (a previous failed run
        # would otherwise cook a gltf pointing at missing files).
        missing = [str(s) for _, s, d in jobs if not d.exists()]
        if missing:
            sys.stderr.write(f"[scanlands] ERROR: {len(missing)} ktx2 missing, "
                             f"re-run without --skip-convert:\n")
            for m in missing[:10]:
                sys.stderr.write(f"  {m}\n")
            return 1

    # 4. Write rewritten gltf.
    with open(stage / args.gltf_name, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=1)
    print(f"[scanlands] wrote {stage / args.gltf_name}")

    if args.skip_pak:
        print("[scanlands] --skip-pak: stopping before PakCooker")
        return 0

    pakcooker = Path(args.pakcooker)
    if not pakcooker.exists():
        sys.stderr.write(f"ERROR: PakCooker not found: {pakcooker}\n")
        return 2
    out_pak = content / "paks" / args.pak_name
    out_pak.parent.mkdir(parents=True, exist_ok=True)
    vfs_model = f"models/{args.scene_dir_name}/{args.gltf_name}"
    cmd = [str(pakcooker),
           "--content-dir", str(content),
           "--in-vfs", vfs_model,
           "--out-pak", str(out_pak),
           "--meshlets", args.meshlets,
           "--lods", args.lods,
           "--lod-ratios", args.lod_ratios,
           "--lod-error", str(args.lod_error),
           "--meshopt", "on",
           "--verify", "on"]
    print(f"[scanlands] running: {' '.join(cmd)}")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.stderr.write(f"[scanlands] PakCooker failed rc={r.returncode}\n")
        return r.returncode
    print(f"[scanlands] pak OK: {out_pak} ({out_pak.stat().st_size / 1e6:.1f} MB)")
    print("[scanlands] no GI bake by design (sky-IBL ambient fallback)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
