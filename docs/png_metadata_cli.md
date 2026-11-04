# Extract metadata embedded in Tourmaline PNGs

Tourmaline snapshot PNGs store a `tsNp` ancillary chunk containing
`TSNP1\\0` followed by versioned UTF-8 JSON. The GPU-free, standard-library-only
`png_metadata.py` command reads the **original per-frame record**, rather than
reconstructing the scene's world state.

```sh
# Print metadata for one PNG as pretty-printed JSON
python tools/png_metadata.py out/frames/Frame_0001.png

# Extract every PNG in a directory as newline-delimited JSON
python tools/png_metadata.py out/frames --jsonl

# Search nested directories and save to a NEW file
python tools/png_metadata.py out/scenario-snapshots --recursive --output metadata.json
```

For one PNG, stdout is `{"path":"...","metadata":{...}}`; for multiple
PNGs, it is a JSON array of these objects. With `--jsonl`, one object is
written per line, useful for pipelines. The `metadata` object preserves all
fields encoded in the file, such as `frame`, `tick`, `events`, `provenance`,
`state` (keyframes) or `patch` (deltas), hashes and RNG details.

The reader reuses `snapshot_sequence.read_snapshot` to validate the PNG
signature, chunk CRCs, the single `tsNp` chunk, JSON format/version and
terminator. It refuses symlinks, corrupted/missing snapshot metadata and
overwriting an output file. It returns exit code 0 on success, 1 on failure,
and does not emit partial stdout if any input is invalid. This tool **does
not** validate decoded pixels, chain continuity or reconstruct deltas.

To verify a complete capture or reconstruct a world state, use the existing
sequence CLI instead:

```sh
python tools/toursnapshot.py verify out/frames --json
python tools/toursnapshot.py extract out/frames --frame 61 --output state.json --json
```

Non-Tourmaline PNGs without a `tsNp` snapshot record are not supported.
