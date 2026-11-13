#!/usr/bin/env python3
"""Tonemap a linear-HDR specular capture (RGBA16F raw) to a viewable PNG.

Matches the renderer's ACES filmic + sRGB chain closely enough to compare
specular-only images against the beauty screenshots. The renderer's
specular export (TOURMALINE_GI_EXPORT_SPECULAR) is the env-specular slice
(ambientSpec x occlusion, linear HDR); this applies the same ACES + gamma
the tonemap pass uses, scaled by exposure.
"""
import argparse
from pathlib import Path

import numpy as np


def tonemap(x, exposure=1.0):
    # ACES filmic (Narkowicz), same shape as the engine tonemap.
    x = np.maximum(x * exposure, 0.0)
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dir", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--exposure", type=float, default=1.0)
    args = p.parse_args(argv)

    import json
    meta = json.loads((args.dir / "meta.json").read_text(encoding="utf-8"))
    w, h = int(meta["width"]), int(meta["height"])
    raw = (args.dir / "irradiance.raw").read_bytes()
    arr = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)[..., :3].astype(np.float64)

    rgb = tonemap(arr, args.exposure)
    rgb = np.clip(rgb, 0.0, 1.0)
    srgb = np.power(rgb, 1.0 / 2.2)  # approximate sRGB gamma
    img = (np.clip(srgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)

    from PIL import Image
    Image.fromarray(img, "RGB").save(args.output)
    print(f"wrote {args.output} ({w}x{h})")


if __name__ == "__main__":
    main()
