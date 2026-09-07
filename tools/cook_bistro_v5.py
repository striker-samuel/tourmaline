#!/usr/bin/env python3
# cook_bistro_v5.py — stage + cook the raw Lumberyard Bistro v5.2 FBX source.
#
# Pipeline:
#   1. Blender (headless) converts BistroExterior.fbx -> glTF separate
#      (.gltf + .bin + ~400 .png, see fbx_to_gltf.py flow). This script takes
#      that export dir as --src-dir.
#   2. Material fixups for the engine (see fix_materials):
#      - Blender leaves metallicFactor absent (spec default 1.0 = fully
#        metallic). Lumberyard v5.2 is dielectric except metals; the FBX
#        specular packing did not survive, so force metallicFactor = 0.0.
#      - Blender marks everything BLEND + doubleSided. The engine has no
#        transparent path for this scene; force OPAQUE, except foliage-like
#        materials which become MASK (cutout) + keep doubleSided.
#      - Drop KHR_materials_specular (engine ignores it; saves ~130 tiny
#        placeholder textures from the cook).
#   3. Convert each referenced .png to .ktx2 with toktx (uastc + zstd,
#      usage-classified srgb/linear/normal, same flags as ktx_bulk_convert).
#   4. Stage app/content/models/Bistro/<gltf,bin,ktx2> with rewritten image
#      URIs, then run PakCooker --meshlets on -> content/paks/bistro.pak.
#
# Usage (PowerShell, from repo root F:/REPOS/vulkantoy):
#   py tools/cook_bistro_v5.py --src-dir C:/Users/samuel/AppData/Local/Temp/opencode/bistro_blender
#
# The Blender export step (one-time, ~2-3 min):
#   & "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe" --background \
#     --python C:/Users/samuel/AppData/Local/Temp/opencode/fbx_to_gltf.py -- \
#     F:/REPOS/Bistro_v5_2/BistroExterior.fbx <src-dir>/bistro.gltf

import argparse
import json
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOKTX = Path(r"C:\Program Files\KTX-Software\bin\toktx.exe")

# Materials whose name contains one of these become alpha-cutout (MASK);
# everything else is forced OPAQUE (Blender exports all as BLEND).
MASK_KEYWORDS = (
    "foliage", "leaves", "leaf", "ivy", "flowers", "flower", "hedges",
    "hedge", "tree", "linde", "plant", "bux",
)


def fix_materials(gltf):
    """Engine-targeted material fixups. Returns (n_metal_fixed, n_mask, n_opaque)."""
    n_metal = n_mask = n_opaque = 0
    for mat in gltf.get("materials", []):
        pbr = mat.setdefault("pbrMetallicRoughness", {})
        # Absent metallicFactor defaults to 1.0 (fully metallic) per spec;
        # force dielectric. Keep Blender's converted roughnessFactor (~0.55).
        if pbr.get("metallicFactor", 1.0) != 0.0:
            pbr["metallicFactor"] = 0.0
            n_metal += 1
        # Drop specular extension (engine ignores it; placeholders only).
        exts = mat.get("extensions", {})
        if "KHR_materials_specular" in exts:
            del exts["KHR_materials_specular"]
        name = mat.get("name", "").lower()
        if any(k in name for k in MASK_KEYWORDS):
            mat["alphaMode"] = "MASK"
            mat["alphaCutoff"] = 0.5
            mat["doubleSided"] = True
            n_mask += 1
        else:
            mat["alphaMode"] = "OPAQUE"
            n_opaque += 1
    # Remove now-unreferenced top-level extension declaration if present.
    used = set()
    for mat in gltf.get("materials", []):
        used.update(mat.get("extensions", {}).keys())
    ext_used = gltf.get("extensionsUsed", [])
    gltf["extensionsUsed"] = [e for e in ext_used if e in used]
    return n_metal, n_mask, n_opaque


def classify_images(gltf):
    """Map image index -> 'srgb' | 'linear' | 'normal' from material usage."""
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
    return kind


def toktx_cmd(toktx, kind, src_png, dst_ktx, uastc_quality, zcmp):
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
    cmd += [str(dst_ktx), str(src_png)]
    return cmd


def convert_one(job):
    """toktx PNG -> ktx2. Returns (uri, ok, msg)."""
    uri, src_png, dst_ktx, kind, toktx, quality, zcmp = job
    try:
        if dst_ktx.exists() and dst_ktx.stat().st_mtime >= src_png.stat().st_mtime:
            return (uri, True, "skip-up-to-date")
        dst_ktx.parent.mkdir(parents=True, exist_ok=True)
        cmd = toktx_cmd(toktx, kind, src_png, dst_ktx, quality, zcmp)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
        if r.returncode != 0:
            return (uri, False, f"toktx rc={r.returncode}: {r.stderr[-2000:]}")
        return (uri, True, "ok")
    except Exception as e:  # noqa: BLE001
        return (uri, False, f"{type(e).__name__}: {e}")


def main():
    ap = argparse.ArgumentParser(description="Stage + cook Lumberyard Bistro v5.2.")
    ap.add_argument("--src-dir", required=True,
                    help="Blender glTF-separate export dir (bistro.gltf + .bin + .png)")
    ap.add_argument("--app-content", default=str(REPO_ROOT / "app" / "content"))
    ap.add_argument("--scene-dir-name", default="Bistro")
    ap.add_argument("--gltf-name", default="bistro.gltf")
    ap.add_argument("--pak-name", default="bistro.pak")
    ap.add_argument("--toktx", default=str(DEFAULT_TOKTX))
    ap.add_argument("--pakcooker", default=str(REPO_ROOT / "build" / "Release" / "PakCooker.exe"))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--uastc-quality", type=int, default=1)
    ap.add_argument("--zcmp", type=int, default=12)
    ap.add_argument("--skip-convert", action="store_true")
    ap.add_argument("--skip-pak", action="store_true")
    ap.add_argument("--meshlets", default="on",
                    help="PakCooker meshlets path: 'on' (default) or 'off'")
    args = ap.parse_args()

    src = Path(args.src_dir)
    content = Path(args.app_content)
    stage = content / "models" / args.scene_dir_name
    src_gltf_path = src / args.gltf_name
    if not src_gltf_path.exists():
        sys.stderr.write(f"ERROR: gltf not found: {src_gltf_path}\n")
        return 2
    toktx = Path(args.toktx)
    if not toktx.exists():
        sys.stderr.write(f"ERROR: toktx not found: {toktx}\n")
        return 2

    with open(src_gltf_path, "r", encoding="utf-8") as f:
        gltf = json.load(f)

    n_metal, n_mask, n_opaque = fix_materials(gltf)
    print(f"[bistro-v5] materials: metallic->0: {n_metal}, mask: {n_mask}, opaque: {n_opaque}")

    images = gltf.get("images", [])
    kinds = classify_images(gltf)
    print(f"[bistro-v5] images={len(images)} classified={len(kinds)}")

    # Collect referenced images + rewrite URIs to staged .ktx2.
    jobs = []
    missing = []
    for idx, img in enumerate(images):
        uri = img.get("uri", "")
        if not uri or uri.startswith("data:"):
            continue
        src_png = (src / Path(uri)).resolve()
        if src_png.suffix.lower() not in (".png", ".jpg", ".jpeg"):
            continue
        if not src_png.exists():
            missing.append(uri)
            continue
        kind = kinds.get(idx, "srgb")
        dst_ktx = stage / (Path(uri).with_suffix(".ktx2").as_posix())
        jobs.append((uri, src_png, dst_ktx, kind, toktx,
                     args.uastc_quality, args.zcmp))
        img["uri"] = Path(uri).with_suffix(".ktx2").as_posix()
        img["mimeType"] = "image/ktx2"

    print(f"[bistro-v5] cookable={len(jobs)} missing={len(missing)}")
    for m in missing[:20]:
        print(f"[bistro-v5]   missing source: {m}")

    if not args.skip_convert:
        jobs.sort(key=lambda j: j[0])
        t0 = time.time()
        ok = fail = skipped = 0
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futs = {pool.submit(convert_one, j): j[0] for j in jobs}
            done = 0
            for fut in as_completed(futs):
                uri, good, msg = fut.result()
                done += 1
                if good:
                    ok += 1
                    if msg == "skip-up-to-date":
                        skipped += 1
                else:
                    fail += 1
                    print(f"[bistro-v5] FAILED {uri}: {msg}")
                if done % 25 == 0 or done == len(jobs):
                    dt = time.time() - t0
                    print(f"[bistro-v5] convert {done}/{len(jobs)} "
                          f"(ok={ok} fail={fail}) {dt:.0f}s")
        print(f"[bistro-v5] convert done: ok={ok} (skipped={skipped}) "
              f"fail={fail} in {time.time()-t0:.0f}s")
        if fail:
            sys.stderr.write(f"[bistro-v5] {fail} conversions failed\n")
            return 1
    else:
        print("[bistro-v5] --skip-convert: reusing staged ktx2 files")

    # Stage buffers + rewritten gltf.
    stage.mkdir(parents=True, exist_ok=True)
    for buf in gltf.get("buffers", []):
        uri = buf.get("uri", "")
        if not uri or uri.startswith("data:"):
            continue
        src_blob = (src / Path(uri)).resolve()
        dst_blob = stage / Path(uri).as_posix()
        if src_blob.exists() and (not dst_blob.exists() or
                                  dst_blob.stat().st_mtime < src_blob.stat().st_mtime):
            print(f"[bistro-v5] copying {uri} ({src_blob.stat().st_size/1e6:.0f} MB)...")
            dst_blob.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_blob, dst_blob)
    with open(stage / args.gltf_name, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=1)
    print(f"[bistro-v5] wrote {stage / args.gltf_name}")

    if args.skip_pak:
        print("[bistro-v5] --skip-pak: stopping before PakCooker")
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
           "--meshopt", "on",
           "--verify", "on"]
    print(f"[bistro-v5] running: {' '.join(cmd)}")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.stderr.write(f"[bistro-v5] PakCooker failed rc={r.returncode}\n")
        return r.returncode
    print(f"[bistro-v5] pak OK: {out_pak} ({out_pak.stat().st_size/1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
