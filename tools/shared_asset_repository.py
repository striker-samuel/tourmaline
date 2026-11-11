"""Shared NAS/LAN asset repository; intentionally separate from cooked project catalog.

Source identity and source revision history live here. Published representations
are immutable build outputs linked to source revisions. Neither concept assigns
or modifies Tourmaline's cooked content IDs, assets.db or project.h.

Run one authoritative scanner on the machine with the protected archive. This
module extends the offline archive without introducing external dependencies.
"""
from __future__ import annotations

from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import hmac
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import threading
import uuid
from urllib.parse import parse_qs, urlsplit

try:
    from tools.offline_asset_service import Archive, handler_for
except ModuleNotFoundError:
    from offline_asset_service import Archive, handler_for


try:
    from tools.asset_read_presence import ConsumerPresence
except ModuleNotFoundError:
    from asset_read_presence import ConsumerPresence

_TOKEN_HEADER = "Bearer "
_HEX_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_PRODUCT_KIND = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}\Z")
_MAX_PUBLISH_BYTES = 8 * 1024**3


class SharedArchive(Archive):
    """A single authoritative source archive and published-products store."""

    def __init__(self, source, archive, cache, settle=2.0, max_cache_bytes=20 * 1024**3):
        super().__init__(source, archive, cache, settle, max_cache_bytes)
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS source_assets (
                    asset_id TEXT NOT NULL PRIMARY KEY,
                    current_path TEXT NOT NULL,
                    missing INTEGER NOT NULL DEFAULT 0,
                    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
                CREATE TABLE IF NOT EXISTS source_aliases (
                    path TEXT NOT NULL PRIMARY KEY,
                    asset_id TEXT NOT NULL REFERENCES source_assets(asset_id));
                CREATE TABLE IF NOT EXISTS source_revisions (
                    asset_id TEXT NOT NULL REFERENCES source_assets(asset_id),
                    revision INTEGER NOT NULL,
                    sha256 TEXT NOT NULL,
                    size INTEGER NOT NULL,
                    source_path TEXT NOT NULL,
                    path_revision INTEGER NOT NULL,
                    captured_at TEXT NOT NULL,
                    PRIMARY KEY(asset_id, revision),
                    UNIQUE(source_path, path_revision));
                CREATE TABLE IF NOT EXISTS published_products (
                    product_id TEXT NOT NULL PRIMARY KEY,
                    source_asset_id TEXT NOT NULL REFERENCES source_assets(asset_id),
                    representation TEXT NOT NULL,
                    variant TEXT NOT NULL DEFAULT '',
                    UNIQUE(source_asset_id, representation, variant));
                CREATE TABLE IF NOT EXISTS published_versions (
                    publish_id TEXT NOT NULL PRIMARY KEY,
                    product_id TEXT NOT NULL REFERENCES published_products(product_id),
                    version INTEGER NOT NULL,
                    source_revision INTEGER NOT NULL,
                    source_sha256 TEXT NOT NULL,
                    artifact_sha256 TEXT NOT NULL,
                    filename TEXT NOT NULL,
                    size INTEGER NOT NULL,
                    published_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                    UNIQUE(product_id, version));
                CREATE INDEX IF NOT EXISTS idx_source_revisions_sha ON source_revisions(sha256);
                CREATE INDEX IF NOT EXISTS idx_published_blob ON published_versions(artifact_sha256);
            """)
        # Backfill a pre-existing offline archive without rescanning the share.
        self.reconcile_identity()

    @staticmethod
    def validate_id(value):
        try:
            parsed = uuid.UUID(value)
        except (ValueError, AttributeError, TypeError) as exc:
            raise ValueError("invalid source asset ID") from exc
        if str(parsed) != value:
            raise ValueError("source asset ID must be canonical UUID")
        return value

    def reconcile_identity(self):
        """Map observed path history to independent, stable source IDs.

        An unambiguous missing-old-path/new-path pair with the same content
        is treated as a rename. Ambiguity is NOT resolved by guesswork:
        duplicate-content files remain separate assets.
        """
        with self.connect() as db:
            files = {r["path"]: dict(r) for r in db.execute(
                "SELECT * FROM files")}
            aliases = {r["path"]: r["asset_id"] for r in db.execute(
                "SELECT * FROM source_aliases")}
            existing = {r["asset_id"]: dict(r) for r in db.execute(
                "SELECT * FROM source_assets")}
            live_paths = {path for path, row in files.items() if not row["missing"]}
            unknown = sorted(live_paths - aliases.keys())
            orphaned = defaultdict(list)
            by_digest = defaultdict(list)
            for asset_id, row in existing.items():
                if row["current_path"] not in live_paths:
                    old = files.get(row["current_path"])
                    if old is not None:
                        orphaned[old["sha256"]].append(asset_id)
            for path in unknown:
                by_digest[files[path]["sha256"]].append(path)
            moves = {}
            for digest, paths in by_digest.items():
                candidates = orphaned.get(digest, [])
                if len(paths) == len(candidates) == 1:
                    moves[paths[0]] = candidates[0]

            for path in unknown:
                asset_id = moves.get(path) or str(uuid.uuid4())
                if asset_id not in existing:
                    db.execute(
                        "INSERT INTO source_assets(asset_id,current_path) VALUES(?,?)",
                        (asset_id, path))
                    existing[asset_id] = {"asset_id": asset_id, "current_path": path}
                db.execute("INSERT INTO source_aliases(path,asset_id) VALUES(?,?)",
                           (path, asset_id))
                aliases[path] = asset_id

            # A moved asset retains its historical path alias, but that alias
            # must not identify a second, simultaneously live file. Keep the
            # identity at its current path and give a reused old path a new ID.
            live_by_id = defaultdict(list)
            for path in sorted(live_paths):
                live_by_id[aliases[path]].append(path)
            for asset_id, paths in live_by_id.items():
                if len(paths) < 2:
                    continue
                current = existing[asset_id]["current_path"]
                keep = current if current in paths else paths[0]
                for path in paths:
                    if path == keep:
                        continue
                    replacement = str(uuid.uuid4())
                    db.execute(
                        "INSERT INTO source_assets(asset_id,current_path) VALUES(?,?)",
                        (replacement, path))
                    db.execute(
                        "UPDATE source_aliases SET asset_id=? WHERE path=?",
                        (replacement, path))
                    aliases[path] = replacement
                    existing[replacement] = {
                        "asset_id": replacement, "current_path": path}

            for path in sorted(live_paths):
                asset_id = aliases[path]
                db.execute("UPDATE source_assets SET current_path=?,missing=0 WHERE asset_id=?",
                           (path, asset_id))

            live_ids = {aliases[path] for path in live_paths}
            for asset_id in existing:
                if asset_id not in live_ids:
                    db.execute("UPDATE source_assets SET missing=1 WHERE asset_id=?",
                               (asset_id,))

            # Legacy path-revisions remain available through /v1. Map them to
            # source revisions, preserving time order as best as possible.
            source_rows = list(db.execute(
                "SELECT path,revision,sha256,size,captured_at FROM versions "
                "ORDER BY captured_at,path,revision"))
            for entry in source_rows:
                path = entry["path"]
                if path not in aliases:
                    # Migrating a deleted file from an old offline archive.
                    asset_id = str(uuid.uuid4())
                    db.execute("INSERT INTO source_assets(asset_id,current_path,missing) "
                               "VALUES(?,?,1)", (asset_id, path))
                    db.execute("INSERT INTO source_aliases(path,asset_id) VALUES(?,?)",
                               (path, asset_id))
                    aliases[path] = asset_id
                asset_id = aliases[path]
                previously = db.execute(
                    "SELECT 1 FROM source_revisions WHERE source_path=? AND path_revision=?",
                    (path, entry["revision"])).fetchone()
                if previously:
                    continue
                last = db.execute(
                    "SELECT revision,sha256 FROM source_revisions WHERE asset_id=? "
                    "ORDER BY revision DESC LIMIT 1", (asset_id,)).fetchone()
                # A pure rename with identical bytes updates current_path
                # without creating an artificial content revision.
                if last and last["sha256"] == entry["sha256"]:
                    continue
                revision = (last["revision"] + 1) if last else 1
                db.execute(
                    "INSERT INTO source_revisions"
                    "(asset_id,revision,sha256,size,source_path,path_revision,captured_at)"
                    "VALUES(?,?,?,?,?,?,?)",
                    (asset_id, revision, entry["sha256"], entry["size"],
                     path, entry["revision"], entry["captured_at"]))

    def scan(self, verify=False):
        result = super().scan(verify=verify)
        self.reconcile_identity()
        return result

    def sources(self):
        with self.connect() as db:
            return [dict(r) for r in db.execute(
                "SELECT a.asset_id,a.current_path,a.missing,a.created_at,"
                "(SELECT MAX(revision) FROM source_revisions r "
                "WHERE r.asset_id=a.asset_id) AS revision "
                "FROM source_assets a ORDER BY a.current_path")]

    def source_revisions(self, asset_id):
        self.validate_id(asset_id)
        with self.connect() as db:
            return [dict(r) for r in db.execute(
                "SELECT * FROM source_revisions WHERE asset_id=? ORDER BY revision DESC",
                (asset_id,))]

    def source_version(self, asset_id, revision=None):
        self.validate_id(asset_id)
        with self.connect() as db:
            if revision is None:
                row = db.execute(
                    "SELECT * FROM source_revisions WHERE asset_id=? "
                    "ORDER BY revision DESC LIMIT 1", (asset_id,)).fetchone()
            else:
                if not isinstance(revision, int) or revision < 1:
                    raise ValueError("revision must be a positive integer")
                row = db.execute(
                    "SELECT * FROM source_revisions WHERE asset_id=? AND revision=?",
                    (asset_id, revision)).fetchone()
            if row is None:
                raise FileNotFoundError("source revision not found")
            return dict(row)

    def products(self, source_asset_id=None):
        if source_asset_id:
            self.validate_id(source_asset_id)
        with self.connect() as db:
            if source_asset_id:
                rows = db.execute(
                    "SELECT p.*,v.publish_id,v.version,v.artifact_sha256,"
                    "v.source_revision,v.source_sha256,v.filename,v.size,v.published_at "
                    "FROM published_products p JOIN published_versions v "
                    "ON p.product_id=v.product_id "
                    "WHERE p.source_asset_id=? ORDER BY p.representation,p.variant,v.version DESC",
                    (source_asset_id,))
            else:
                rows = db.execute(
                    "SELECT p.*,v.publish_id,v.version,v.artifact_sha256,"
                    "v.source_revision,v.source_sha256,v.filename,v.size,v.published_at "
                    "FROM published_products p JOIN published_versions v "
                    "ON p.product_id=v.product_id "
                    "ORDER BY p.source_asset_id,p.representation,p.variant,v.version DESC")
            return [dict(r) for r in rows]

    def publish(self, asset_id, revision, representation, variant, filename, stream, size):
        self.validate_id(asset_id)
        source = self.source_version(asset_id, revision)
        if not _PRODUCT_KIND.fullmatch(representation):
            raise ValueError("invalid representation name")
        if variant and not _PRODUCT_KIND.fullmatch(variant):
            raise ValueError("invalid representation variant")
        if (not filename or filename in (".", "..") or len(filename) > 255
                or "/" in filename or "\\" in filename or "\x00" in filename):
            raise ValueError("filename must be a single basename")
        if not 0 <= size <= _MAX_PUBLISH_BYTES:
            raise ValueError("invalid upload size")
        fd, temp = tempfile.mkstemp(prefix=".publish-", dir=self.archive)
        digest = hashlib.sha256()
        remaining = size
        try:
            with os.fdopen(fd, "wb") as out:
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError("upload ended before Content-Length")
                    digest.update(chunk)
                    out.write(chunk)
                    remaining -= len(chunk)
                out.flush()
                os.fsync(out.fileno())
            sha = digest.hexdigest()
            blob = self.blob(self.archive, sha)
            self.install_blob(temp, blob, sha)
            product_id = str(uuid.uuid5(uuid.NAMESPACE_URL,
                "tourmaline-product:" + asset_id + ":" + representation + ":" + variant))
            publish_id = str(uuid.uuid4())
            with self.connect() as db:
                # Serialize allocation of product version numbers across
                # concurrent HTTP threads (and separate processes).
                db.execute("BEGIN IMMEDIATE")
                db.execute(
                    "INSERT OR IGNORE INTO published_products"
                    "(product_id,source_asset_id,representation,variant) VALUES(?,?,?,?)",
                    (product_id,asset_id,representation,variant))
                version = db.execute(
                    "SELECT COALESCE(MAX(version),0)+1 FROM published_versions "
                    "WHERE product_id=?", (product_id,)).fetchone()[0]
                db.execute(
                    "INSERT INTO published_versions"
                    "(publish_id,product_id,version,source_revision,source_sha256,"
                    "artifact_sha256,filename,size) VALUES(?,?,?,?,?,?,?,?)",
                    (publish_id,product_id,version,revision,source["sha256"],
                     sha,filename,size))
            return {"publish_id": publish_id, "product_id": product_id,
                    "version": version, "source_asset_id": asset_id,
                    "source_revision": revision, "source_sha256": source["sha256"],
                    "representation": representation, "variant": variant,
                    "artifact_sha256": sha, "filename": filename, "size": size}
        finally:
            Path(temp).unlink(missing_ok=True)

    def blob_owners(self, sha):
        """List all source UUIDs referencing a content-addressed blob.

        A deduplicated digest may be shared across separate source assets.
        """
        if not _HEX_DIGEST.fullmatch(sha):
            raise ValueError("invalid content digest")
        with self.connect() as db:
            rows = db.execute(
                "SELECT asset_id FROM source_revisions WHERE sha256=? "
                "UNION SELECT p.source_asset_id FROM published_versions v "
                "JOIN published_products p ON p.product_id=v.product_id "
                "WHERE v.artifact_sha256=?", (sha, sha))
            return [r[0] for r in rows]

    def product_source_id(self, product_id):
        with self.connect() as db:
            row = db.execute(
                "SELECT source_asset_id FROM published_products WHERE product_id=?",
                (product_id,)).fetchone()
        if row is None:
            raise FileNotFoundError("product not found")
        return row[0]

    def legacy_path_source_id(self, path):
        from tools.offline_asset_service import relative_asset_path
        path = relative_asset_path(path)
        with self.connect() as db:
            row = db.execute("SELECT asset_id FROM source_aliases WHERE path=?",
                             (path,)).fetchone()
        if row is None:
            raise FileNotFoundError("source not found")
        return row[0]

    def referenced_blob(self, sha):
        if not _HEX_DIGEST.fullmatch(sha):
            raise ValueError("invalid content digest")
        with self.connect() as db:
            a = db.execute("SELECT 1 FROM source_revisions WHERE sha256=? LIMIT 1",
                           (sha,)).fetchone()
            b = db.execute("SELECT 1 FROM published_versions WHERE artifact_sha256=? LIMIT 1",
                           (sha,)).fetchone()
        if not a and not b:
            raise FileNotFoundError("artifact is not published or archived")
        path = self.blob(self.archive, sha)
        if not path.is_file():
            raise FileNotFoundError("archive blob missing")
        return path


def handler_for_shared(store, token, consumer_ttl_seconds=90, registry=None):
    """Bearer-protected API, with opt-in per-user permission enforcement.

    When registry is enabled the legacy shared bearer token is NOT accepted.
    This prevents the old deployment-wide secret bypassing user permissions.
    """
    base = handler_for(store)
    token = str(token)
    if len(token) < 24:
        raise ValueError("shared service access token must be at least 24 characters")
    readers = ConsumerPresence(store, ttl_seconds=consumer_ttl_seconds)

    class Handler(base):
        def setup(self):
            super().setup()
            self.connection.settimeout(30)

        def identity(self):
            if hasattr(self, "_resolved_user"):
                return self._resolved_user
            header = self.headers.get("Authorization", "")
            if registry is None:
                user = "legacy" if hmac.compare_digest(
                    header, _TOKEN_HEADER + token) else None
            else:
                # One registry snapshot per request prevents repeated disk reads
                # for large asset listings and gives consistent ACL decisions.
                self._permission_snapshot = registry.load()
                user = registry.identity(
                    header[len(_TOKEN_HEADER):],
                    snapshot=self._permission_snapshot) if header.startswith(
                        _TOKEN_HEADER) else None
            self._resolved_user = user
            return user

        def authorized(self, permission, asset_id=None):
            username = self.identity()
            if username is None:
                self.reply({"error": "unauthorized"}, 401)
                return False
            if registry is not None and not registry.allows(
                    username, permission, asset_id,
                    snapshot=self._permission_snapshot):
                self.reply({"error": "forbidden", "permission": permission}, 403)
                return False
            return True

        def visible(self, asset_id, permission="source.read"):
            return registry is None or registry.allows(
                self.identity(), permission, asset_id,
                snapshot=self._permission_snapshot)

        def get_permission(self, path):
            if path in ("/", "/asset", "/v1/assets", "/v1/versions",
                        "/v1/status", "/v1/resolve", "/v2/sources",
                        "/v2/resolve") or (
                            path.startswith("/v2/sources/") and path.endswith("/revisions")):
                return "source.read"
            if path == "/v2/products":
                return "product.read"
            if path.startswith("/v2/blobs/"):
                return "artifact.read"
            if path in ("/v2/consumers", "/v2/open-assets"):
                return "presence.read"
            return "source.read"

        def do_GET(self):
            # Static dashboard contains no asset data and never embeds a
            # credential. Browser users enter their token into the page.
            if urlsplit(self.path).path == "/consumers":
                page = (Path(__file__).with_name("asset_consumers_dashboard.html")
                        .read_text(encoding="utf-8"))
                return self.reply(page, kind="text/html")
            request_path = urlsplit(self.path).path
            if request_path != "/health" and not self.authorized(
                    self.get_permission(request_path)):
                return
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            try:
                if url.path in ("/v2/consumers", "/v2/open-assets"):
                    kind = query.get("resource_kind", [None])[0]
                    ident = query.get("resource_id", [None])[0]
                    if url.path == "/v2/open-assets":
                        report = readers.open_assets(kind, ident)
                        if registry is not None:
                            report["open_assets"] = [entry for entry in report["open_assets"]
                                if self.visible(
                                    entry["id"] if entry["kind"] == "source" else
                                    store.product_source_id(entry["id"]),
                                    "source.read" if entry["kind"] == "source" else "product.read")]
                        for field in ("username", "workstation", "mode"):
                            selected = query.get(field, [None])[0]
                            if selected is not None:
                                report["open_assets"] = [entry
                                    for entry in report["open_assets"]
                                    if entry[field] == selected]
                        return self.reply(report)
                    result = readers.active(kind, ident)
                    if registry is not None:
                        for session in result["consumers"]:
                            session["reads"] = [read for read in session["reads"]
                                if self.visible(
                                    read["id"] if read["kind"] == "source" else
                                    store.product_source_id(read["id"]),
                                    "source.read" if read["kind"] == "source" else "product.read")]
                        result["consumers"] = [session for session in result["consumers"]
                                               if session["reads"]]
                    return self.reply(result)
                if url.path == "/v2/sources":
                    return self.reply({"sources": [
                        asset for asset in store.sources()
                        if self.visible(asset["asset_id"])]})
                if url.path.startswith("/v2/sources/") and url.path.endswith("/revisions"):
                    asset_id = url.path.split("/")[3]
                    if not self.visible(asset_id):
                        return self.reply({"error": "not found"}, 404)
                    return self.reply({"revisions": store.source_revisions(asset_id)})
                if url.path == "/v2/resolve":
                    raw = query.get("revision", [None])[0]
                    value = int(raw) if raw else None
                    asset_id = query.get("asset_id", [""])[0]
                    if not self.visible(asset_id):
                        return self.reply({"error": "not found"}, 404)
                    item = store.source_version(asset_id, value)
                    return self.reply(item)
                if url.path == "/v2/products":
                    source_id = query.get("source_asset_id", [None])[0]
                    if source_id and not self.visible(source_id, "product.read"):
                        return self.reply({"error": "not found"}, 404)
                    return self.reply({"products": [
                        row for row in store.products(source_id)
                        if self.visible(row["source_asset_id"], "product.read")]})
                if url.path.startswith("/v2/blobs/"):
                    sha = url.path[len("/v2/blobs/"):]
                    if "/" in sha:
                        raise ValueError("invalid content digest")
                    owners = store.blob_owners(sha)
                    if not owners:
                        raise FileNotFoundError("artifact not found")
                    if not any(self.visible(owner, "artifact.read") for owner in owners):
                        return self.reply({"error": "not found"}, 404)
                    artifact = store.referenced_blob(sha)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(artifact.stat().st_size))
                    self.send_header("X-Content-SHA256", sha)
                    self.send_header("Cache-Control", "private, immutable, max-age=31536000")
                    self.send_header("Vary", "Authorization")
                    self.send_header("X-Content-Type-Options", "nosniff")
                    self.end_headers()
                    with artifact.open("rb") as fd:
                        shutil.copyfileobj(fd, self.wfile, length=1024 * 1024)
                    return
            except (ValueError, TypeError) as exc:
                return self.reply({"error": str(exc)}, 400)
            except FileNotFoundError as exc:
                return self.reply({"error": str(exc)}, 404)
            except OSError as exc:
                return self.reply({"error": str(exc)}, 500)
            # Legacy browser/resolve APIs expose host-local paths, so do not
            # expose them to ACL users until they have filtered v2 equivalents.
            if registry is not None and url.path in (
                    "/", "/asset", "/v1/assets", "/v1/versions",
                    "/v1/resolve", "/v1/status"):
                return self.reply({"error": "use filtered v2 API"}, 403)
            return super().do_GET()

        def do_POST(self):
            if not self.authorized("presence.write"):
                return
            path = urlsplit(self.path).path
            if path not in ("/v2/consumers/heartbeat", "/v2/consumers/release"):
                return self.reply({"error": "not found"}, 404)
            try:
                length = int(self.headers.get("Content-Length", "-1"))
                if length < 0 or length > 65536:
                    raise ValueError("body must be a JSON object of at most 65536 bytes")
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError("body must be a JSON object")
                if path.endswith("/heartbeat"):
                    if registry is not None:
                        for read in readers._normalize(body.get("reads")):
                            kind, ident, version, mode = read
                            source_id = ident if kind == "source" else store.product_source_id(ident)
                            if not self.visible(source_id,
                                    "source.read" if kind == "source" else "product.read"):
                                return self.reply({"error": "forbidden"}, 403)
                            if mode == "publish" and not self.visible(
                                    source_id, "product.publish"):
                                return self.reply({"error": "forbidden"}, 403)
                    result = readers.heartbeat(
                        body.get("session_id"), body.get("label"),
                        body.get("kind"), body.get("reads"),
                        username=self.identity() if registry is not None else None,
                        workstation=body.get("workstation", ""))
                else:
                    result = readers.release(
                        body.get("session_id"),
                        username=self.identity() if registry is not None else None)
                return self.reply(result)
            except PermissionError as exc:
                return self.reply({"error": str(exc)}, 403)
            except (ValueError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                return self.reply({"error": str(exc)}, 400)
            except OSError as exc:
                return self.reply({"error": str(exc)}, 500)

        def do_PUT(self):
            if not self.authorized("product.publish"):
                return
            if not self.authorized("product.publish",
                    self.headers.get("X-Source-Asset-ID", "")):
                return
            if urlsplit(self.path).path != "/v2/publish":
                return self.reply({"error": "not found"}, 404)
            try:
                content_length = int(self.headers.get("Content-Length", "-1"))
                revision = int(self.headers.get("X-Source-Revision", "0"))
                result = store.publish(
                    self.headers.get("X-Source-Asset-ID", ""), revision,
                    self.headers.get("X-Representation", ""),
                    self.headers.get("X-Variant", ""),
                    self.headers.get("X-Filename", ""),
                    self.rfile, content_length)
                return self.reply(result, 201)
            except (ValueError, TypeError) as exc:
                return self.reply({"error": str(exc)}, 400)
            except FileNotFoundError as exc:
                return self.reply({"error": str(exc)}, 404)
            except OSError as exc:
                return self.reply({"error": str(exc)}, 500)

    return Handler


def serve_shared(store, token, bind="127.0.0.1", port=8765, poll=3.0,
                 consumer_ttl_seconds=90, registry=None):
    if not token or len(token) < 24:
        raise ValueError("access_token of at least 24 characters required")
    if bind not in ("127.0.0.1", "0.0.0.0"):
        raise ValueError("bind must be 127.0.0.1 or 0.0.0.0")
    stop = threading.Event()

    def watch():
        while not stop.is_set():
            try:
                store.scan()
            except OSError as exc:
                print("Shared source scan unavailable:", exc, flush=True)
            stop.wait(max(0.5, poll))

    watcher = threading.Thread(target=watch, daemon=True)
    watcher.start()
    try:
        with ThreadingHTTPServer((bind, port), handler_for_shared(
                store, token, consumer_ttl_seconds, registry=registry)) as server:
            if bind == "0.0.0.0":
                print("SECURITY WARNING: this server speaks HTTP, not TLS. "
                      "Terminate authenticated remote traffic at an HTTPS "
                      "reverse proxy and firewall the backend port.", flush=True)
            print(f"Tourmaline shared asset repository listening on {bind}:{port}",
                  flush=True)
            server.serve_forever()
    finally:
        stop.set()
        watcher.join(timeout=5)
