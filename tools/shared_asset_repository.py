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

            for path in sorted(live_paths):
                # If a previously deleted path is reused, preserve its path
                # identity. The system cannot infer creator intent from bytes.
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

    def scan(self):
        result = super().scan()
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
            blob.parent.mkdir(parents=True, exist_ok=True)
            if not blob.exists():
                os.replace(temp, blob)
            product_id = str(uuid.uuid5(uuid.NAMESPACE_URL,
                "tourmaline-product:" + asset_id + ":" + representation + ":" + variant))
            publish_id = str(uuid.uuid4())
            with self.connect() as db:
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


def handler_for_shared(store, token):
    """Bearer-protected LAN endpoints + compatibility with /v1 browser API."""
    base = handler_for(store)
    token = str(token)
    if len(token) < 24:
        raise ValueError("shared service access token must be at least 24 characters")

    class Handler(base):
        def authenticated(self):
            return hmac.compare_digest(
                self.headers.get("Authorization", ""), _TOKEN_HEADER + token)

        def deny(self):
            self.reply({"error": "unauthorized"}, 401)

        def do_GET(self):
            if not self.authenticated() and urlsplit(self.path).path != "/health":
                return self.deny()
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            try:
                if url.path == "/v2/sources":
                    return self.reply({"sources": store.sources()})
                if url.path.startswith("/v2/sources/") and url.path.endswith("/revisions"):
                    asset_id = url.path.split("/")[3]
                    return self.reply({"revisions": store.source_revisions(asset_id)})
                if url.path == "/v2/resolve":
                    raw = query.get("revision", [None])[0]
                    value = int(raw) if raw else None
                    item = store.source_version(query.get("asset_id", [""])[0], value)
                    return self.reply(item)
                if url.path == "/v2/products":
                    return self.reply({"products": store.products(
                        query.get("source_asset_id", [None])[0])})
                if url.path.startswith("/v2/blobs/"):
                    sha = url.path[len("/v2/blobs/"):]
                    if "/" in sha:
                        raise ValueError("invalid content digest")
                    artifact = store.referenced_blob(sha)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(artifact.stat().st_size))
                    self.send_header("X-Content-SHA256", sha)
                    self.send_header("Cache-Control", "public, immutable, max-age=31536000")
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
            return super().do_GET()

        def do_PUT(self):
            if not self.authenticated():
                return self.deny()
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


def serve_shared(store, token, bind="127.0.0.1", port=8765, poll=3.0):
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
        with ThreadingHTTPServer((bind, port), handler_for_shared(store, token)) as server:
            print(f"Tourmaline shared asset repository listening on {bind}:{port}",
                  flush=True)
            server.serve_forever()
    finally:
        stop.set()
        watcher.join(timeout=5)
