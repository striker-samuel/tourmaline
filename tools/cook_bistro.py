#!/usr/bin/env python3
# cook_bistro.py — stage + cook the Niagara/Lumberyard Bistro scene.
#
# The upstream bistro.gltf references .png images (which don't exist on disk;
# only sibling .dds files do) plus MSFT_texture_dds alternates. The runtime /
# PakCooker pipeline only accepts .ktx2, so this script:
#   1. Decodes each referenced .dds via Pillow to a temp .png,
#   2. Encodes it to .ktx2 with toktx (uastc + zstd, glTF-driven srgb/linear/
#      normal classification, same flags as tools/ktx_bulk_convert.py),
#   3. Stages models/Bistro/<original tree> with .ktx2 textures + a rewritten
#      bistro.gltf whose .png image URIs point at the .ktx2 files,
#   4. Stamps SMVK_materials_shadows {"castShadow": false} on decal overlay
#      materials (diffuse texture name contains "decal") so their flat
#      quads never darken the geometry they sit on, and on lamp envelope
#      materials (bulb/glass/small fixtures) so a lamp mesh never
#      occludes its own light source (the bake drops no-cast materials
#      from the trace scene; runtime shadow passes skip them) — the
#      engine reads the extension at import; paks round-trip it into
#      castsShadows,
#   5. Stamps extras {"smvk.staticLight": true} on the glTF point-light
#      nodes (the lamp set: 96 KHR_lights_punctual points) so the GI
#      bakes harvest them as NEE sources (CollectStaticBakeLights); the
#      authored Sun directional stays runtime-only (it seeds the env,
#      see GltfPunctualAimAxis/BuildCookedLightsFromWorld),
#   6. Runs PakCooker --meshlets on --lods on to produce content/paks/bistro.pak.
#
# Usage (PowerShell, from repo root F:/REPOS/vulkantoy):
#   py tools/cook_bistro.py --niagara-dir F:/REPOS/niagara_bistro
#       [--app-content F:/content] [--mode uastc] [--workers 8]
#       [--skip-convert] [--skip-pak]
#
# --skip-convert reuses already-staged .ktx2 files (timestamp check does this
# automatically too). --skip-pak stops after staging (lets you run PakCooker
# manually).

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOKTX = Path(r"C:\Program Files\KTX-Software\bin\toktx.exe")


def classify_images(gltf):
    """Map image index -> 'srgb' | 'linear' | 'normal' from material usage.

    Mirrors tools/ktx_bulk_convert.py plus the bistro-required
    KHR_materials_pbrSpecularGlossiness diffuse/specular inputs.
    First writer wins (baseColor/emissive outrank metallic/roughness only if
    seen first — in practice an image is used for one purpose).
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
        # MSFT_texture_dds alternate of the same texture shares the purpose.
        ext = tex.get("extensions", {}).get("MSFT_texture_dds", {})
        dsrc = ext.get("source", -1)
        if dsrc is not None and 0 <= dsrc < len(images):
            kind.setdefault(dsrc, k)

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


def stamp_shadow_casting(gltf):
    """SMVK_materials_shadows stamping pass.

    Two families get {"castShadow": false}:
      - decal overlay materials (diffuse texture name contains
        "decal", e.g. street_decals/edge_decals/wall_decals): flat
        quads must not darken the geometry they sit on.
      - lamp envelope materials (bulb/glass/small fixtures): a lamp
        mesh must never occlude its own light source. In the bake the
        no-cast material leaves the trace scene entirely; runtime
        shadow passes skip it. Poles/housings keep casting.

    Applies on top of the authored alphaModes: BLEND decals already
    skip the caster path; this also covers OPAQUE/MASK materials
    sharing decal atlases. Returns (decal, lamp) stamped counts."""
    def all_uris(mat):
        uris = []
        def push(tex_index):
            if not isinstance(tex_index, int):
                return
            textures = gltf.get("textures", [])
            if tex_index < 0 or tex_index >= len(textures):
                return
            src = textures[tex_index].get("source", -1)
            images = gltf.get("images", [])
            if src is not None and 0 <= src < len(images):
                uris.append(images[src].get("uri", ""))
            ext = textures[tex_index].get("extensions", {}).get(
                "MSFT_texture_dds", {})
            dsrc = ext.get("source", -1)
            if dsrc is not None and 0 <= dsrc < len(images):
                uris.append(images[dsrc].get("uri", ""))
        pbr = mat.get("pbrMetallicRoughness", {})
        if isinstance(pbr, dict):
            if isinstance(pbr.get("baseColorTexture"), dict):
                push(pbr["baseColorTexture"].get("index"))
            if isinstance(pbr.get("emissiveTexture"), dict):
                push(pbr["emissiveTexture"].get("index"))
        for slot in ("normalTexture", "emissiveTexture",
                     "occlusionTexture"):
            if isinstance(mat.get(slot), dict):
                push(mat[slot].get("index"))
        sg = mat.get("extensions", {}).get(
            "KHR_materials_pbrSpecularGlossiness", {})
        if isinstance(sg, dict):
            if isinstance(sg.get("diffuseTexture"), dict):
                push(sg["diffuseTexture"].get("index"))
            if isinstance(sg.get("specularGlossinessTexture"), dict):
                push(sg["specularGlossinessTexture"].get("index"))
        return uris

    def stamp(mat):
        mat.setdefault("extensions", {})["SMVK_materials_shadows"] = {
            "castShadow": False,
        }

    decals = lamps = 0
    for mat in gltf.get("materials", []):
        uris = all_uris(mat)
        uri_blob = " ".join(uris).lower()
        name = (mat.get("name") or "").lower()
        if any("decal" in u for u in uris):
            stamp(mat)
            decals += 1
            continue
        # Lamp envelopes: streetlight glass/bulb/support, bistro exterior
        # spotlight glass/bulb/lens, lanterns, wall lights, ceiling
        # lamps, string-light fixtures. Poles and housings (streetlight
        # "metal", spotlight "main") keep casting.
        lamp = False
        if "streetlight" in uri_blob and (
                "glass" in uri_blob or "bulb" in uri_blob):
            lamp = True  # paris_streetlight_01 glass/bulb/support_bulb
        elif "bistroexteriorspotlight" in uri_blob and any(
                k in name for k in ("glass", "bulb", "emissive", "focus")):
            lamp = True  # spotlight glass_focus/glass/bulb_light/emissive/focus
        elif ("lantern" in uri_blob or "wall_light" in uri_blob
                or "ceiling_lamp" in uri_blob):
            lamp = True  # whole small fixtures (emissive lamp meshes)
        elif "stringlights" in uri_blob and "fixtures" in name:
            lamp = True  # string-light bulbs on wires
        if lamp:
            stamp(mat)
            lamps += 1
    return decals, lamps


def stamp_static_lights(gltf):
    """Stamp extras {"smvk.staticLight": true} on KHR_lights_punctual
    POINT-light nodes (the bistro lamp set). The engine harvests
    static-flagged lights into GI/lightmap bakes (NEE sources) and the
    runtime forward+ set skips them, so a lamp's direct + bounced term
    is carried by the baked sources. The Sun directional stays
    runtime-only: it seeds the env (sky disc + CSM), and the bake sun
    comes from the baker's --sun-* arguments instead. Returns the
    stamped node count."""
    stamped = 0
    for node in gltf.get("nodes", []):
        ext = node.get("extensions", {}).get("KHR_lights_punctual", {})
        light_idx = ext.get("light")
        if not isinstance(light_idx, int):
            continue
        lights = gltf.get("extensions", {}).get(
            "KHR_lights_punctual", {}).get("lights", [])
        if light_idx < 0 or light_idx >= len(lights):
            continue
        if lights[light_idx].get("type") != "point":
            continue
        extras = node.setdefault("extras", {})
        if not isinstance(extras, dict):
            continue
        if extras.get("smvk.staticLight") is True:
            continue  # already authored static
        extras["smvk.staticLight"] = True
        stamped += 1
    return stamped


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
    """Decode DDS -> (green-flip normals) -> temp PNG -> toktx -> ktx2.
    Returns (uri, ok, msg)."""
    from PIL import Image

    uri, src_dds, dst_ktx, kind, toktx, quality, zcmp = job
    try:
        if dst_ktx.exists() and dst_ktx.stat().st_mtime >= src_dds.stat().st_mtime:
            return (uri, True, "skip-up-to-date")
        dst_ktx.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(src_dds) as im:
            im.load()
            if im.mode not in ("RGB", "RGBA"):
                im = im.convert("RGBA")
            if kind == "normal":
                # Lumberyard ships DirectX-style normal maps (green = -Y).
                # The engine's TBN follows the glTF/OpenGL convention
                # (green = +Y, validated against sponza): flip green.
                if im.mode != "RGBA":
                    im = im.convert("RGBA")
                r, g, b, a = im.split()
                g = g.point(lambda v: 255 - v)
                im = Image.merge("RGBA", (r, g, b, a))
            with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
                tmp_path = tmp.name
            try:
                im.save(tmp_path)
            except Exception:
                os.unlink(tmp_path)
                raise
        try:
            cmd = toktx_cmd(toktx, kind, tmp_path, dst_ktx, quality, zcmp)
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
            if r.returncode != 0:
                return (uri, False, f"toktx rc={r.returncode}: {r.stderr[-2000:]}")
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
        return (uri, True, "ok")
    except Exception as e:  # noqa: BLE001
        return (uri, False, f"{type(e).__name__}: {e}")


def validate_staged(stage, ktx_bin="ktx"):
    """Delete staged .ktx2 files that fail `ktx validate` (e.g. torn writes
    from a killed run — they would otherwise pass the mtime check forever).
    Returns (valid, removed)."""
    import shutil as _shutil
    exe = _shutil.which(ktx_bin) or ktx_bin
    valid = removed = 0
    for ktx in sorted(stage.rglob("*.ktx2")):
        try:
            r = subprocess.run([exe, "validate", str(ktx)],
                               capture_output=True, timeout=120)
            if r.returncode == 0:
                valid += 1
            else:
                ktx.unlink()
                removed += 1
                print(f"[bistro] removed invalid: {ktx.relative_to(stage)}")
        except Exception as e:  # noqa: BLE001
            print(f"[bistro] validate error on {ktx.name}: {e}")
    return valid, removed


def main():
    ap = argparse.ArgumentParser(description="Stage + cook Niagara Bistro.")
    ap.add_argument("--niagara-dir", default="F:/REPOS/niagara_bistro")
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
    ap.add_argument("--shard", type=int, default=0,
                    help="1-based shard index to convert (0 = all)")
    ap.add_argument("--num-shards", type=int, default=1)
    args = ap.parse_args()

    niagara = Path(args.niagara_dir)
    content = Path(args.app_content)
    stage = content / "models" / args.scene_dir_name
    src_gltf_path = niagara / args.gltf_name
    if not src_gltf_path.exists():
        sys.stderr.write(f"ERROR: gltf not found: {src_gltf_path}\n")
        return 2
    toktx = Path(args.toktx)
    if not toktx.exists():
        sys.stderr.write(f"ERROR: toktx not found: {toktx}\n")
        return 2

    with open(src_gltf_path, "r", encoding="utf-8") as f:
        gltf = json.load(f)
    images = gltf.get("images", [])
    kinds = classify_images(gltf)
    print(f"[bistro] images={len(images)} classified={len(kinds)}")
    n_decal, n_lamp = stamp_shadow_casting(gltf)
    print(f"[bistro] no-cast-shadow: decals={n_decal} lamp envelopes={n_lamp}")
    n_static = stamp_static_lights(gltf)
    print(f"[bistro] static bake lights stamped: {n_static} point-light nodes")

    # Collect .png-referenced images (the cookable set) + their DDS source.
    jobs = []
    missing = []
    rewritten = 0
    for idx, img in enumerate(images):
        uri = img.get("uri", "")
        if not uri or uri.startswith("data:"):
            continue
        if not uri.lower().endswith(".png"):
            continue  # .dds alternates: never referenced by the loader, skip
        dds_uri = uri[:-4] + ".dds"
        src_dds = niagara / Path(dds_uri)
        dst_ktx = stage / (Path(uri).with_suffix(".ktx2").as_posix())
        if not src_dds.exists():
            missing.append(uri)
            continue
        kind = kinds.get(idx, "srgb")
        jobs.append((uri, src_dds, dst_ktx, kind, toktx,
                     args.uastc_quality, args.zcmp))
        # Rewrite the gltf image entry to the staged ktx2 (POSIX uri).
        img["uri"] = Path(uri).with_suffix(".ktx2").as_posix()
        img["mimeType"] = "image/ktx2"
        rewritten += 1

    print(f"[bistro] cookable={len(jobs)} missing-dds={len(missing)} rewritten={rewritten}")
    for m in missing[:20]:
        print(f"[bistro]   missing DDS for: {m}")
    if missing:
        print(f"[bistro] WARNING: {len(missing)} textures have no DDS source "
              f"(materials using them render untextured)")

    if not args.skip_convert:
        try:
            from PIL import Image  # noqa: F401
        except ImportError:
            sys.stderr.write("ERROR: Pillow required (py -m pip install Pillow)\n")
            return 2
        jobs.sort(key=lambda j: j[0])
        if args.num_shards > 1:
            if args.shard < 1 or args.shard > args.num_shards:
                sys.stderr.write("ERROR: --shard must be in [1, --num-shards]\n")
                return 2
            jobs = [j for i, j in enumerate(jobs)
                    if i % args.num_shards == args.shard - 1]
            print(f"[bistro] shard {args.shard}/{args.num_shards}: "
                  f"{len(jobs)} textures")
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
                    print(f"[bistro] FAILED {uri}: {msg}")
                if done % 25 == 0 or done == len(jobs):
                    dt = time.time() - t0
                    print(f"[bistro] convert {done}/{len(jobs)} "
                          f"(ok={ok} fail={fail}) {dt:.0f}s")
        print(f"[bistro] convert done: ok={ok} (skipped={skipped}) "
              f"fail={fail} in {time.time()-t0:.0f}s")
        if fail:
            sys.stderr.write(f"[bistro] {fail} conversions failed\n")
            return 1
    else:
        print("[bistro] --skip-convert: reusing staged ktx2 files")

    # NOTE: the RootNode 100x scale (FBX cm->m) and Level -90d X (DCC
    # Z-up -> glTF Y-up) shims are KEPT VERBATIM. They are load-bearing:
    # world AABB with shims is 111x32x119 m, without 1.1x1.2x0.3 m. The
    # old neutralization here predated the node-transform conjugation fix
    # in engine/src/Utils/gltf.cpp (C*M*C) and produced a tiny scene on
    # its side. No node transform rewriting happens at cook time.

    # Copy buffers + write rewritten gltf.
    stage.mkdir(parents=True, exist_ok=True)
    for blob in ("bistro.bin", "bistro-anim.bin"):
        src = niagara / blob
        dst = stage / blob
        if src.exists() and (not dst.exists() or
                             dst.stat().st_mtime < src.stat().st_mtime):
            print(f"[bistro] copying {blob} ({src.stat().st_size/1e6:.0f} MB)...")
            shutil.copy2(src, dst)
    # Copy any other loose files the gltf references directly (.bin only;
    # textures are generated above).
    with open(stage / args.gltf_name, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=1)
    print(f"[bistro] wrote {stage / args.gltf_name}")

    if args.skip_pak:
        print("[bistro] --skip-pak: stopping before PakCooker")
        return 0
    if args.num_shards > 1:
        print("[bistro] sharded run: stopping before PakCooker "
              "(run without --shard/--num-shards --skip-convert to cook the pak)")
        return 0

    # Full run: a killed earlier run may have left torn .ktx2 files with fresh
    # mtimes — validate and reconvert whatever is missing/invalid.
    valid, removed = validate_staged(stage)
    print(f"[bistro] staged valid={valid} removed-invalid={removed}")
    if removed:
        print("[bistro] re-converting repaired set...")
        jobs_todo = [j for j in jobs
                     if not j[2].exists()]
        if jobs_todo:
            from PIL import Image  # noqa: F401
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                for uri, good, msg in pool.map(convert_one, jobs_todo):
                    if not good:
                        sys.stderr.write(f"[bistro] FAILED {uri}: {msg}\n")
                        return 1
            print(f"[bistro] repaired {len(jobs_todo)} textures")

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
    print(f"[bistro] running: {' '.join(cmd)}")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.stderr.write(f"[bistro] PakCooker failed rc={r.returncode}\n")
        return r.returncode
    print(f"[bistro] pak OK: {out_pak} ({out_pak.stat().st_size/1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())