# Agent CLI manual: inspection, pixels and comparisons

This is the discoverable CLI entry point for coding agents. Read it before
inspecting cooked content or interpreting rendering regression results.
Commands are run from the **repository root**. Paths below are examples.

## Principles

- Prefer headless queries. Do not launch the editor, GUI pak browser, or
  Vulkan renderer just to enumerate packaged content.
- stdout carries results; stderr carries diagnostics. Use \`--json\` for one
  structured document and \`--jsonl\` for a stream of independent records.
- Quote patterns (especially \`*\`, \`**\`, \`?\`, \`|\`) so the shell does not
  expand them before the tool receives them.
- A zero exit code means success; inspect nonzero exits rather than ignoring
  them after pipelines. Bash: \`set -o pipefail\`. PowerShell: check
  \`$LASTEXITCODE\` for native processes. Empty matches are not corruption.
- \`.tapp\` is an appmanifest v2 **release pointer**; \`#ids\` asset identities
  live in each SMP8 \`.pak\`. Distinguish logical assets (\`assets\`) from
  physical TOC pages (\`entries\`); one logical identity need not map to
  a single TOC page.
- Listing metadata is cheap relative to decoding payloads; \`validate
  --deep\` explicitly walks every stored page CRC.
- Tools do not modify source assets or cooked packages. The project
  pipeline's \`Project ids\` and \`Project explain\` may reconcile asset
  registry state: they are **not** equivalent to these read-only queries.

## tourinfo: package explorer

\`tools/tourinfo.py\` reuses the SMP8 and IDTR readers/decoders in
\`tools/pak_browser.py\`. Python standard library is sufficient for package
metadata, identities, and CRC validation; optional \`pip install lz4\`
is needed to inspect a compressed payload. No Qt or Vulkan needed.

\`\`\`bash
python tools/tourinfo.py build/content/paks/game.tapp
python tools/tourinfo.py summary build/content/paks/game.tapp --section entry,paks,kinds --json
python tools/tourinfo.py paks build/content/paks/game.tapp --json
python tools/tourinfo.py assets build/content/paks/game.tapp --kind texture -g 'textures/**' --jsonl
python tools/tourinfo.py entries build/content/paks/game.tapp --regex 'terrain|foliage' -i --paths
python tools/tourinfo.py entries build/content/paks/game.tapp --kind mesh --sort size_stored --desc --limit 15
python tools/tourinfo.py inspect build/content/paks/game.tapp --path '#ids' --jsonl
python tools/tourinfo.py validate build/content/paks/game.tapp --deep --json
\`\`\`

\`-g/--glob\` matches the **entire logical path**: \`*\` stops at \`/\`,
\`**\` crosses directories, and \`**/\` also matches zero directories.
\`--regex\` searches the logical path (Python regular expressions);
\`-i\` affects both. Repeated filters within a family are OR; glob,
regex and kind families combine with AND. \`--kind\` accepts repeated
or comma-separated names; \`--fields\` selects record columns.
\`--sort\`, \`--desc\`, and \`--limit\` constrain output.

For Unix pipelines:

\`\`\`bash
python tools/tourinfo.py entries game.tapp --jsonl |
  jq -r 'select(.kind == "texture") | .path'
python tools/tourinfo.py assets game.tapp --paths |
  rg -i 'metal|roughness|normal'
python tools/tourinfo.py entries game.tapp -g '**/normal*' --jsonl |
  python tools/tourinfo.py inspect --stdin --jsonl |
  jq -c 'select(.decoded.ktxMagicOk == false)'
\`\`\`

\`inspect --stdin\` accepts a JSONL record with \`pak\` and one of
\`slot\`, \`path\`, or \`id\`; the \`entries --jsonl\` output contains
both \`pak\` and \`slot\` for unambiguous inspection. Other records
may not resolve one-to-one; prefer slots when inspecting pages.

Data shape: \`assets\` carry catalog \`id\`, \`kind\`, \`path\`, \`scene\`,
pak name and mount index; \`entries\` carry page \`slot\`, \`offset\`,
\`size_stored\`, \`codec\`, \`content_crc\`, and VFS visibility. JSON
record commands emit \`{"schema_version":1,"command":"...","items":[...]}\`;
JSONL emits one record per line. A single summary/validation emits a
JSON object. The schema is v1; integrations should use field names, not
text-column positions. CLI errors are emitted to stderr, exit 2; deep
validation failure exits 1. \`--jsonl\` is line-delimited output, not a
promise of O(1) archive memory use: current parser reads each pak image.

The old \`python tools/pak_browser.py --cli-dump|--cli-validate <path>\`
remains available and is useful as a compatibility reference. The
optional GUI needs PyQt6. See \`docs/pak_vnext.md\`.

## Project and provenance tooling

\`\`\`bash
Project validate --project ./mygame --recipe game.tourrecipe
Project status --project ./mygame
Project explain --project ./mygame --recipe game.tourrecipe --id 0x1234
\`\`\`

These commands are part of the C++ project pipeline (see
\`tools/project/main.cpp\`, \`docs/content_as_code.md\`).
\`Project validate\` resolves ownership and mount order without cooking;
\`Project status\` reports imported-source freshness. \`explain\` resolves
the asset's origin and stale causes but **may reconcile/tombstone registry
entries**, so do not run it as a supposedly pure read-only operation.

## Pixel inspection / screenshot comparisons

Use \`tools/pixel_compare.py\` when comparing images cross-platform.
It uses Pillow to decode RGB pixels. PNG byte hashes are not a reliable
pixel comparison: two PNGs may encode identical pixels differently.

\`\`\`bash
python -m pip install pillow
python tools/pixel_compare.py before.png after.png --json
python tools/pixel_compare.py before.png after.png --stride 1 --threshold 12 --diff-out out/diff.png
printf 'before.png\tafter.png\n' | python tools/pixel_compare.py --stdin --jsonl |
  jq 'select(.difference_fraction > 0.01)'
\`\`\`

The threshold measures the *sum* of absolute RGB channel deltas per
sampled pixel (range 0..765). Default threshold is 6, stride is 2
(sample every other row and column); stride 1 examines every pixel.
\`--white-min N\` also counts reference pixels with every channel >= N.
JSON output includes decoded RGB SHA-256 for each image, dimensions,
sample counts, changed fraction, average delta and an optional map path.
The map marks changed sampled pixels red. Zero = no threshold-exceeding
pixel differences, 1 = differences, 2 = invalid input/IO, 3 = no Pillow.
Image dimensions must match.

**Existing Windows tool:** \`tools/compare_screenshots.ps1\` is
PowerShell/System.Drawing-only and remains supported on Windows for
historical runs, \`-Reference\`, \`-Compare\`, \`-Threshold\`, \`-Stride\`,
\`-WhiteCount\`, and \`-DiffOut\`. Prefer the new Python tool for
JSONL pipelines or cross-platform automation; do not silently change
the threshold or stride while comparing baseline results.

**Golden capture:** \`tools/golden_capture.py\` launches App to produce
a *fresh* screenshot, hashes **decoded pixels**, and compares against
GPU/driver-keyed \`tools/golden_hashes.txt\`. It needs a working GPU and
Pillow. Run it for GPU regression gates, not the static two-image
comparator. Update baselines only after reviewing visual differences.

**Benchmark comparison:** \`tools/perf_harness.py compare --baseline
<result.json> --candidate <result.json> --max-regression 5\`
compares medians of per-run average frame times while refusing
incompatible work configurations. It is not a pixel comparison. Its
\`cull\` mode compares exact cull verdict JSON baselines (see
\`docs/perf_harness.md\`).

**HZB visualization:** \`tools/hzb_csv_to_images_v2.py\` converts
captured HZB values to images, reports per-mip statistics, and can
verify MIN reduction. It is a diagnostic visualizer rather than an
A/B screenshot comparator; its NumPy/Pillow dependencies are separate
from \`tourinfo\`.

## Testing

\`\`\`bash
python -m unittest discover -s tests -p 'test_tourinfo.py' -v
python -m unittest discover -s tests -p 'test_pak_browser.py' -v
python -m unittest discover -s tests -p 'test_pixel_compare.py' -v
python -m unittest discover -s tools/tests -v
\`\`\`

The first three are CPU-only, with image tests requiring Pillow; the
existing perf harness unit tests do not require a GPU. A production
rendering regression gate additionally needs a valid graphical
environment, matching scene and driver, and a fresh capture.
