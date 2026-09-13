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
#      - Blender marks everything BLEND + doubleSided. Force OPAQUE, except
#        foliage-like materials which become MASK (cutout) + keep
#        doubleSided, and glass materials (KHR_materials_transmission or
#        frosted/frozen/decal in the name) which become BLEND transparent
#        + doubleSided with alpha derived from (1 - transmissionFactor).
#      - Decal overlay materials (diffuse texture name contains "decal",
#        e.g. street_decals/edge_decals) get SMVK_materials_shadows
#        {"castShadow": false}: their flat quads must not darken the
#        geometry they sit on. The engine importer reads the extension;
#        the pak round-trips it into Material::castsShadows.
#      - Drop KHR_materials_specular (engine ignores it; saves ~130 tiny
#        placeholder textures from the cook). Transmission is kept as the
#        glass marker.
#   3. Convert each referenced .png to .ktx2 with toktx (uastc + zstd,
#      usage-classified srgb/linear/normal, same flags as ktx_bulk_convert).
#   4. Stage F:/content/models/Bistro/<gltf,bin,ktx2> with rewritten image
#      URIs, then run PakCooker --meshlets on -> F:/content/paks/bistro.pak.
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

# Materials whose name contains one of these become alpha-cutout (MASK).
# "forge" covers the wrought-iron window frames/grilles next to the glass.
MASK_KEYWORDS = (
    "foliage", "leaves", "leaf", "ivy", "flowers", "flower", "hedges",
    "hedge", "tree", "linde", "plant", "bux", "forge",
)

# Glass materials become alpha-blended transparent (BLEND). The engine
# has no transmission path, so transmissive glass (windows) is
# approximated with alpha blend: double-sided, depth-test/no-write and
# back-to-front sorted via the Transparent draw set (no shadow or
# depth-prepass caster, bypasses the opaque/masked gfx-filter buckets).
# Name fallback covers non-transmissive window glass (frosted/frozen) and
# decals. Deliberately NOT a broad "glass" match: that would also catch
# cutout frames (forge_windows_01, glass_ornament) and opaque contents
# (foam, liquid, wine), which must keep their MASK/OPAQUE modes.
BLEND_KEYWORDS = (
    "frosted", "frozen", "decal",
)


def _transmission_factor(mat):
    exts = mat.get("extensions", {})
    tm = exts.get("KHR_materials_transmission", {})
    if isinstance(tm, dict):
        try:
            return float(tm.get("transmissionFactor", 0.0))
        except (TypeError, ValueError):
            return 0.0
    return 0.0


def _set_glass_alpha(mat, pbr, exts, alpha):
    """Stamp a < 1 alpha without breaking the spec-gloss color fallback.

    The engine prefers the core baseColorFactor when present and falls
    back to the KHR_materials_pbrSpecularGlossiness diffuseFactor/
    diffuseTexture otherwise (see engine/src/Utils/gltf.cpp). Writing a
    core factor onto a spec-gloss-only material would shadow its texture
    (and a bare metallicFactor stamp would defeat the glossiness ->
    roughness mapping, which keys off untouched core defaults), so: core
    factor when one already exists, spec-gloss diffuseFactor when that
    carries the color, and a fresh dielectric core block only when the
    material has no color anywhere (e.g. transmission-only glass).
    Existing alphas < 1 are always kept.
    """
    bcf = pbr.get("baseColorFactor") if isinstance(pbr, dict) else None
    if isinstance(bcf, list) and len(bcf) >= 4:
        if bcf[3] >= 0.999:
            bcf[3] = alpha
        return
    sg = exts.get("KHR_materials_pbrSpecularGlossiness")
    if isinstance(sg, dict):
        df = sg.get("diffuseFactor")
        if isinstance(df, list) and len(df) >= 4:
            if df[3] >= 0.999:
                df[3] = alpha
            return
    if pbr is None:
        pbr = {}
        mat["pbrMetallicRoughness"] = pbr
    if "baseColorFactor" not in pbr:
        pbr["baseColorFactor"] = [1.0, 1.0, 1.0, alpha]
        pbr["metallicFactor"] = 0.0


def _diffuse_image_uri(gltf, mat):
    """Image uri of a material's diffuse color (core baseColorTexture,
    spec-gloss diffuseTexture fallback — mirrors the engine's baseColor
    resolution in gltf.cpp)."""
    def image_uri(tex_index):
        if not isinstance(tex_index, int):
            return None
        textures = gltf.get("textures", [])
        if tex_index < 0 or tex_index >= len(textures):
            return None
        src = textures[tex_index].get("source", -1)
        images = gltf.get("images", [])
        if src is not None and 0 <= src < len(images):
            return images[src].get("uri", "")
        return None

    pbr = mat.get("pbrMetallicRoughness")
    if isinstance(pbr, dict) and isinstance(pbr.get("baseColorTexture"), dict):
        uri = image_uri(pbr["baseColorTexture"].get("index"))
        if uri:
            return uri
    sg = mat.get("extensions", {}).get("KHR_materials_pbrSpecularGlossiness")
    if isinstance(sg, dict) and isinstance(sg.get("diffuseTexture"), dict):
        return image_uri(sg["diffuseTexture"].get("index"))
    return None


def fix_materials(gltf):
    """Engine-targeted material fixups. Returns (n_metal_fixed, n_mask, n_blend, n_opaque, n_nocast)."""
    n_metal = n_mask = n_blend = n_opaque = n_nocast = 0
    for mat in gltf.get("materials", []):
        # Absent metallicFactor defaults to 1.0 (fully metallic) per spec;
        # force dielectric on materials that already carry a core block.
        # NOTE: never create the block just to stamp this: a fresh stub
        # would defeat the engine's spec-gloss fallback (glossiness ->
        # roughness keys off untouched core defaults). Pbr-less glass gets
        # its dielectric core from _set_glass_alpha instead.
        pbr = mat.get("pbrMetallicRoughness")
        if isinstance(pbr, dict) and pbr.get("metallicFactor", 1.0) != 0.0:
            pbr["metallicFactor"] = 0.0
            n_metal += 1
        # Drop specular extension (engine ignores it; placeholders only).
        # Transmission is kept: it marks glass for the BLEND mapping below
        # (and the engine importer reads it too for direct-gltf loads).
        exts = mat.get("extensions", {})
        if "KHR_materials_specular" in exts:
            del exts["KHR_materials_specular"]
        name = mat.get("name", "").lower()
        transmission = _transmission_factor(mat)
        if transmission > 0.001 or any(k in name for k in BLEND_KEYWORDS):
            # Transparent glass: alpha derives from (1 - transmission) so
            # clear panes (~0.9) read ~0.1 and dirty panes (~0.66) ~0.34.
            # Decals keep their factors (texture alpha drives); only
            # non-transmissive window glass (frosted/frozen) falls back
            # to 0.25.
            mat["alphaMode"] = "BLEND"
            mat["doubleSided"] = True
            if transmission > 0.001:
                _set_glass_alpha(mat, pbr, exts,
                                 min(max(1.0 - transmission, 0.05), 1.0))
            elif any(k in name for k in ("frosted", "frozen")):
                _set_glass_alpha(mat, pbr, exts, 0.25)
            n_blend += 1
        elif any(k in name for k in MASK_KEYWORDS):
            mat["alphaMode"] = "MASK"
            mat["alphaCutoff"] = 0.5
            mat["doubleSided"] = True
            n_mask += 1
        else:
            mat["alphaMode"] = "OPAQUE"
            n_opaque += 1
        # Decal overlays (poster/trim/grime quads whose diffuse texture is
        # named ...decal...) never cast shadows: the flat quad's full-quad
        # depth darkens whatever it sits on. Applies on top of the alpha
        # classification above (BLEND decals already skip the caster path;
        # this also covers the OPAQUE edge-decal trims and MASK materials
        # sharing decal atlases).
        diffuse_uri = _diffuse_image_uri(gltf, mat)
        if diffuse_uri and "decal" in diffuse_uri.lower():
            mat.setdefault("extensions", {})["SMVK_materials_shadows"] = {
                "castShadow": False,
            }
            n_nocast += 1
    # Remove now-unreferenced top-level extension declaration if present.
    used = set()
    for mat in gltf.get("materials", []):
        used.update(mat.get("extensions", {}).keys())
    ext_used = gltf.get("extensionsUsed", [])
    gltf["extensionsUsed"] = [e for e in ext_used if e in used]
    return n_metal, n_mask, n_blend, n_opaque, n_nocast


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
    ap.add_argument("--app-content", default=r"F:/content")
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
    ap.add_argument("--lods", default="on",
                    help="PakCooker LOD chain: 'on' (default) or 'off'")
    ap.add_argument("--lod-ratios", default="0.6,0.3,0.12",
                    help="Triangle-count ratios vs LOD0 for PakCooker --lod-ratios")
    ap.add_argument("--lod-error", type=float, default=0.02,
                    help="Relative simplifier error budget for PakCooker --lod-error")
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

    n_metal, n_mask, n_blend, n_opaque, n_nocast = fix_materials(gltf)
    print(f"[bistro-v5] materials: metallic->0: {n_metal}, mask: {n_mask}, "
          f"blend: {n_blend}, opaque: {n_opaque}, no-cast-shadow: {n_nocast}")

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
           "--lods", args.lods,
           "--lod-ratios", args.lod_ratios,
           "--lod-error", str(args.lod_error),
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
