#!/usr/bin/env python3
"""Package a linear-HDR diffuse-irradiance capture into a .npz for
tools/quality_compare.py.

The renderer (TOURMALINE_GI_EXPORT_IRRADIANCE=<dir>) dumps the envSpec MRT as
`<dir>/irradiance.raw` (RGBA16F: E.rgb + covered.a) plus `<dir>/meta.json`.
This converts the raw half floats to float32 and writes the arrays
quality_compare.py expects:
    irradiance  (H,W,3) float32  linear diffuse irradiance
    valid_mask  (H,W)   uint8   1 = a baked source covered the pixel
"""
import argparse
import json
from pathlib import Path

import numpy as np


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dir", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args(argv)

    meta = json.loads((args.dir / "meta.json").read_text(encoding="utf-8"))
    w, h = int(meta["width"]), int(meta["height"])
    if meta.get("format") != "RGBA16F":
        raise ValueError(f"unsupported capture format: {meta.get('format')}")

    raw = (args.dir / "irradiance.raw").read_bytes()
    expected = w * h * 4 * 2
    if len(raw) != expected:
        raise ValueError(f"irradiance.raw is {len(raw)} bytes, expected {expected}")

    arr = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)
    irradiance = arr[..., :3].astype(np.float32)
    valid_mask = (arr[..., 3] > 0.5).astype(np.uint8)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output, irradiance=irradiance, valid_mask=valid_mask)
    print(f"wrote {args.output} ({w}x{h})")


if __name__ == "__main__":
    main()
