# Multi-setup scenario correctness

Use the existing scenario catalog and GPU harness on each machine, with local JSON records by default. No remote execution or new engine integration is needed. Python standard library only.

## Local mode

Edit benchmarks/scenario_matrix.example.json to select scenarios and specify executable paths (Windows: Benchmark.exe). A setup has a unique name, exe, optional workdir, optional TOURMALINE_* environment overrides, and optional mode: cull, snapshot or bench.

    python tools/scenario_matrix.py --matrix benchmarks/scenario_matrix.example.json --machine gpu-01

The default cull mode validates GPU culling against the existing committed benchmarks/cull_baselines/<scenario>.json. Exit 0 means a match, 2 a semantic mismatch; other errors fail closed. Use --baseline-dir for a separately reviewed set of cull baselines. Changing scene or culling-related pins may require separate baselines: do not silently update them. Use perf_harness.py cull --update and review any intentional baseline changes.

Snapshot mode only checks successful capture and reconstruction of the expected sequence. It does NOT verify pixel correctness. For visual equivalence use toursnapshot.py compare-sequence or pixel_compare.py against curated references. Bench mode measures performance, NOT correctness.

Each run creates a unique directory under out/scenario-matrix/<machine>/<setup>/<scenario>/<uuid>/ containing the original result.json and logs, matrix.log, and record.json (machine, platform, setup, scenario, code ID, status, source harness result). Pass --code-id to identify the source revision; default is GITHUB_SHA or unknown. Supply a safe short --machine identifier. Setup paths resolve from the current working directory; run from repository root.

## Optional collector

Start on a trusted host (set TOURMALINE_RECORD_TOKEN to a long random secret):

    python tools/scenario_collector.py --host 0.0.0.0 --port 8765 --output out/scenario-records

Then run each worker with --collector http://collector.lan:8765, keeping TOURMALINE_RECORD_TOKEN in its environment. No --collector means no network traffic from this tool. The server defaults to 127.0.0.1 and rejects non-loopback binding without a token. It supports GET /health and POST /records, validates identifiers, caps JSON at 2 MiB, and writes with exclusive filenames (duplicates return HTTP 409). Failed uploads do not remove local records and cause a nonzero exit.

HTTP does not encrypt bearer tokens: use only on a trusted LAN, or put an HTTPS reverse proxy with access controls in front. Never expose plain HTTP to the public internet.

Keep identical scenario catalogs, cooked assets, and source revisions across machines. For GPU correctness comparisons, use the same scenario pins and reviewed baseline before comparing hardware differences.

CPU tests: python -m unittest discover -s tools/tests -v
