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
    from tools.asset_permissions import PermissionRegistry
except ModuleNotFoundError:
    from asset_read_presence import ConsumerPresence
    from shared_asset_repository import SharedArchive, serve_shared
    from asset_permissions import PermissionRegistry


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
    users = sub.add_parser("users", help="manage protected users and group permissions")
    user_cmd = users.add_subparsers(dest="user_command", required=True)
    user_cmd.add_parser("init", help="create registry and bootstrap admin token")
    user_cmd.add_parser("list", help="show accounts without exposing tokens")
    group_set = user_cmd.add_parser("group-set", help="set group permission rules")
    group_set.add_argument("group")
    group_set.add_argument("--permission", action="append", default=[])
    user_set = user_cmd.add_parser("set", help="create/update a user's groups and rules")
    user_set.add_argument("username")
    user_set.add_argument("--group", action="append", default=[])
    user_set.add_argument("--permission", action="append", default=[])
    rotate = user_cmd.add_parser("rotate-token", help="revoke old token and issue a new one")
    rotate.add_argument("username")
    asset = user_cmd.add_parser("asset-set", help="set per-asset user/group ACL rules")
    asset.add_argument("source_asset_id")
    asset.add_argument("--user")
    asset.add_argument("--group")
    asset.add_argument("--permission", action="append", default=[])
    clear = user_cmd.add_parser("asset-clear", help="remove per-asset ACL restrictions")
    clear.add_argument("source_asset_id")
    args = parser.parse_args(argv)
    settings = load(args.config)
    registry_location = settings.get("permissions_registry")
    if registry_location:
        registry_path = Path(registry_location).expanduser().resolve()
        source_root = Path(settings["source"]).expanduser().resolve()
        if registry_path == source_root or source_root in registry_path.parents:
            parser.error("permissions registry must not be inside the source share")
    else:
        registry_path = None
    if args.command == "users":
        if registry_path is None:
            parser.error("set permissions_registry in the server config first")
        if args.user_command == "init":
            admin_token = PermissionRegistry.initialize(registry_path)
            print("Registry created:", registry_path)
            print("Admin token (store securely; not shown again):", admin_token)
            return 0
        registry = PermissionRegistry(registry_path)
        if args.user_command == "list":
            print(json.dumps(registry.users(), indent=2))
        elif args.user_command == "group-set":
            registry.group_set(args.group, args.permission)
            print("Group updated:", args.group)
        elif args.user_command == "set":
            new_token = registry.user_set(args.username, args.group, args.permission)
            print("User configured:", args.username)
            if new_token:
                print("User token (store securely; not shown again):", new_token)
        elif args.user_command == "rotate-token":
            print("New token (store securely; not shown again):",
                  registry.rotate_token(args.username))
        elif args.user_command == "asset-set":
            if bool(args.user) == bool(args.group):
                parser.error("specify exactly one of --user or --group")
            registry.asset_set(args.source_asset_id,
                               "users" if args.user else "groups",
                               args.user or args.group, args.permission)
            print("Asset ACL updated:", args.source_asset_id)
        elif args.user_command == "asset-clear":
            registry.asset_clear(args.source_asset_id)
            print("Asset ACL removed:", args.source_asset_id)
        return 0
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
                         consumer_ttl_seconds=float(settings.get("consumer_ttl_seconds", 90)),
                         registry=PermissionRegistry(registry_path) if registry_path else None)
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
