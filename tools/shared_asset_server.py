"""Serve the authoritative Tourmaline source/product repository.

Windows: py -3 tools/shared_asset_server.py --config CONFIG serve
Linux: python3 tools/shared_asset_server.py --config CONFIG serve

One scanner/archive per shared project. Workstation caches live separately.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

try:
    from tools.asset_read_presence import ConsumerPresence
    from tools.shared_asset_repository import SharedArchive, serve_shared
except ModuleNotFoundError:
    from asset_read_presence import ConsumerPresence
    from shared_asset_repository import SharedArchive, serve_shared


def load(config):
    data = json.loads(Path(config).expanduser().read_text(encoding="utf-8"))
    if data.get("schema_version") != 2 or data.get("mode") != "shared":
        raise ValueError("expected Tourmaline shared repository config schema 2")
    if len(data.get("access_token", "")) < 24:
        raise ValueError("access_token must be at least 24 characters")
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description="Tourmaline shared NAS asset repository")
    parser.add_argument("--config", required=True)
    sub = parser.add_subparsers(dest="command")
    for cmd in ("serve", "scan", "sources", "products", "status", "consumers"):
        sub.add_parser(cmd)
    versions = sub.add_parser("versions")
    versions.add_argument("source_asset_id")
    publish = sub.add_parser("publish")
    publish.add_argument("source_asset_id")
    publish.add_argument("source_revision", type=int)
    publish.add_argument("representation")
    publish.add_argument("file")
    publish.add_argument("--variant", default="")
    args = parser.parse_args(argv)
    settings = load(args.config)
    store = SharedArchive(settings["source"], settings["archive"], settings["cache"],
                          settle=settings.get("settle_seconds", 2),
                          max_cache_bytes=int(settings.get("cache_gib", 20) * 1024**3))
    action = args.command or "serve"
    if action == "serve":
        try:
            serve_shared(store, settings["access_token"],
                         bind=settings.get("bind", "127.0.0.1"),
                         port=int(settings.get("port", 8765)),
                         poll=float(settings.get("poll_seconds", 3)),
                         consumer_ttl_seconds=float(settings.get("consumer_ttl_seconds", 90)))
        except KeyboardInterrupt:
            return 0
    elif action == "scan":
        print(json.dumps(store.scan(), indent=2))
    elif action == "sources":
        print(json.dumps(store.sources(), indent=2))
    elif action == "versions":
        print(json.dumps(store.source_revisions(args.source_asset_id), indent=2))
    elif action == "products":
        print(json.dumps(store.products(), indent=2))
    elif action == "status":
        print(json.dumps(store.status(), indent=2))
    elif action == "consumers":
        readers = ConsumerPresence(store, float(settings.get("consumer_ttl_seconds", 90)))
        print(json.dumps(readers.active(), indent=2))
    elif action == "publish":
        path = Path(args.file)
        if not path.is_file() or path.is_symlink():
            parser.error("product must be a regular local file")
        with path.open("rb") as fd:
            result = store.publish(args.source_asset_id, args.source_revision,
                                   args.representation, args.variant,
                                   path.name, fd, path.stat().st_size)
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
