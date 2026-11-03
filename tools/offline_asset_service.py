"""Offline-first, whole-file revision capture for an unrestricted SMB/local share.

The source folder is never modified. Archive and cache must live outside it.
Only settled states observed by polling can be guaranteed; rapid intermediate
overwrites need host-level snapshots or filesystem interception.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from html import escape
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sqlite3
import tempfile
import threading
import time
from urllib.parse import parse_qs, quote, urlsplit
import argparse

EXCLUDE_DIRS = frozenset({".git", ".tourmaline", ".asset-service", "__pycache__"})
TEMP_ENDINGS = (".tmp", ".part", ".swp", ".swx", "~")


def is_inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def relative_asset_path(path: str) -> str:
    if not isinstance(path, str) or not path or "\\" in path or "\x00" in path:
        raise ValueError("use a nonempty forward-slash project-relative path")
    if (PurePosixPath(path).is_absolute() or ":" in path
            or any(part in ("", ".", "..") for part in path.split("/"))):
        raise ValueError("asset path must not escape the project")
    return path

class Archive:
    def __init__(self, source: str, archive: str, cache: str,
                 settle: float = 2.0, max_cache_bytes: int = 20 * 1024**3):
        self.source = Path(source).expanduser().resolve()
        self.archive = Path(archive).expanduser().resolve()
        self.cache = Path(cache).expanduser().resolve()
        # Local revision access must survive an offline/unmounted SMB share.
        # Scan reports a transient error, but the archive can still serve
        # already captured versions after restarting the service.
        if self.source.exists() and not self.source.is_dir():
            raise ValueError(f"source must be a directory: {self.source}")
        for a, b in ((self.source, self.archive), (self.source, self.cache),
                     (self.archive, self.cache)):
            if is_inside(a, b) or is_inside(b, a):
                raise ValueError("source, archive and cache must be separate folders")
        self.settle = max(0.0, float(settle))
        self.max_cache_bytes = max(0, int(max_cache_bytes))
        for directory in (self.archive / "blobs", self.cache / "blobs"):
            directory.mkdir(parents=True, exist_ok=True)
        self.db_path = self.archive / "history.sqlite3"
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS files (
                    path TEXT PRIMARY KEY, size INTEGER NOT NULL,
                    mtime_ns INTEGER NOT NULL, sha256 TEXT NOT NULL,
                    missing INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS versions (
                    path TEXT NOT NULL, revision INTEGER NOT NULL,
                    sha256 TEXT NOT NULL, size INTEGER NOT NULL,
                    captured_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                    PRIMARY KEY(path, revision));
                CREATE TABLE IF NOT EXISTS cache (
                    sha256 TEXT NOT NULL, extension TEXT NOT NULL,
                    size INTEGER NOT NULL, accessed REAL NOT NULL,
                    pinned INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY(sha256, extension));
            """)

    @contextmanager
    def connect(self):
        # sqlite3.Connection's context manager commits but does NOT close.
        # Explicit close matters on Windows, where open DB files cannot be
        # removed and long-lived handles can pin WAL files indefinitely.
        db = sqlite3.connect(self.db_path, timeout=30)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA busy_timeout=30000")
            db.execute("PRAGMA journal_mode=WAL")
            yield db
            db.commit()
        except BaseException:
            db.rollback()
            raise
        finally:
            db.close()

    @staticmethod
    def blob(base: Path, digest: str, extension: str = "") -> Path:
        return base / "blobs" / digest[:2] / (digest + extension)

    @staticmethod
    def digest(path: Path) -> str:
        h = hashlib.sha256()
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()

    def discover(self):
        # No constraints on artist directory layout or filename extensions.
        for folder, dirs, names in os.walk(self.source, followlinks=False):
            dirs[:] = [d for d in sorted(dirs) if d not in EXCLUDE_DIRS
                       and not (Path(folder) / d).is_symlink()]
            for name in sorted(names):
                if name.startswith("~$") or name.endswith(TEMP_ENDINGS):
                    continue
                path = Path(folder) / name
                if path.is_file() and not path.is_symlink():
                    yield path.relative_to(self.source).as_posix(), path

    def capture(self, path: Path):
        # Snapshot a complete copy into the private archive before indexing it.
        if path.is_symlink() or not is_inside(path, self.source):
            return None
        before = path.stat()
        if time.time() - before.st_mtime < self.settle:
            return None
        fd, temp = tempfile.mkstemp(prefix=".capture-", dir=self.archive)
        h = hashlib.sha256()
        copied = 0
        try:
            with os.fdopen(fd, "wb") as output, path.open("rb") as input_file:
                for block in iter(lambda: input_file.read(1024 * 1024), b""):
                    output.write(block)
                    h.update(block)
                    copied += len(block)
                output.flush()
                os.fsync(output.fileno())
            after = path.stat()
            if ((before.st_size, before.st_mtime_ns, before.st_ino) !=
                    (after.st_size, after.st_mtime_ns, after.st_ino)
                    or copied != after.st_size):
                return None
            digest = h.hexdigest()
            target = self.blob(self.archive, digest)
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                os.replace(temp, target)
            return digest, copied
        except OSError:
            return None
        finally:
            Path(temp).unlink(missing_ok=True)

    def scan(self):
        if not self.source.is_dir():
            raise OSError("share unavailable; previous revisions were retained")
        outcome = {"captured": 0, "unchanged": 0, "pending": 0, "missing": 0}
        seen = set()
        with self.connect() as db:
            known = {r["path"]: r for r in db.execute("SELECT * FROM files")}
            for rel, path in self.discover():
                seen.add(rel)
                try:
                    st = path.stat()
                    previous = known.get(rel)
                    if (previous is not None and not previous["missing"]
                            and previous["size"] == st.st_size
                            and previous["mtime_ns"] == st.st_mtime_ns):
                        outcome["unchanged"] += 1
                        continue
                    item = self.capture(path)
                    if item is None:
                        outcome["pending"] += 1
                        continue
                    digest, length = item
                    after = path.stat()
                    if (after.st_size, after.st_mtime_ns, after.st_ino) != (
                            st.st_size, st.st_mtime_ns, st.st_ino):
                        outcome["pending"] += 1
                        continue
                except OSError:
                    outcome["pending"] += 1
                    continue
                if previous is None or previous["sha256"] != digest or previous["missing"]:
                    revision = db.execute(
                        "SELECT COALESCE(MAX(revision),0)+1 FROM versions WHERE path=?",
                        (rel,)).fetchone()[0]
                    db.execute("INSERT INTO versions(path,revision,sha256,size) VALUES(?,?,?,?)",
                               (rel, revision, digest, length))
                    outcome["captured"] += 1
                else:
                    outcome["unchanged"] += 1
                db.execute("""INSERT INTO files(path,size,mtime_ns,sha256,missing)
                    VALUES(?,?,?,?,0) ON CONFLICT(path) DO UPDATE SET
                    size=excluded.size,mtime_ns=excluded.mtime_ns,
                    sha256=excluded.sha256,missing=0""",
                    (rel, after.st_size, after.st_mtime_ns, digest))
            for missing in known.keys() - seen:
                if not known[missing]["missing"]:
                    db.execute("UPDATE files SET missing=1 WHERE path=?", (missing,))
                    outcome["missing"] += 1
        return outcome

    def assets(self):
        with self.connect() as db:
            return [dict(r) for r in db.execute("""
                SELECT path,missing,size,sha256,
                  (SELECT MAX(revision) FROM versions v WHERE v.path=f.path) AS revision
                FROM files f ORDER BY path""")]

    def versions(self, path):
        path = relative_asset_path(path)
        with self.connect() as db:
            return [dict(r) for r in db.execute(
                "SELECT * FROM versions WHERE path=? ORDER BY revision DESC", (path,))]

    def resolve(self, path, revision=None, *, pin=False):
        path = relative_asset_path(path)
        with self.connect() as db:
            if revision is None:
                row = db.execute("SELECT * FROM versions WHERE path=? ORDER BY revision DESC LIMIT 1",
                                 (path,)).fetchone()
            else:
                row = db.execute("SELECT * FROM versions WHERE path=? AND revision=?",
                                 (path, int(revision))).fetchone()
            if row is None:
                raise FileNotFoundError(f"missing revision: {path} @ {revision}")
            item = dict(row)
            src = self.blob(self.archive, row["sha256"])
            if not src.is_file():
                raise FileNotFoundError("archived content is missing")
            ext = PurePosixPath(path).suffix.lower()
            dest = self.blob(self.cache, row["sha256"], ext)
            if not dest.is_file() or self.digest(dest) != row["sha256"]:
                dest.parent.mkdir(parents=True, exist_ok=True)
                fd, temp = tempfile.mkstemp(dir=dest.parent, prefix=".cache-")
                try:
                    with os.fdopen(fd, "wb") as output, src.open("rb") as input_file:
                        shutil.copyfileobj(input_file, output, length=1024 * 1024)
                    if self.digest(Path(temp)) != row["sha256"]:
                        raise OSError("archive SHA-256 mismatch")
                    os.replace(temp, dest)
                finally:
                    Path(temp).unlink(missing_ok=True)
            db.execute("""INSERT INTO cache(sha256,extension,size,accessed,pinned)
                VALUES(?,?,?,?,?) ON CONFLICT(sha256,extension) DO UPDATE SET
                accessed=excluded.accessed,pinned=MAX(pinned,excluded.pinned)""",
                (row["sha256"], ext, row["size"], time.time(), int(pin)))
            entries = list(db.execute("SELECT * FROM cache ORDER BY accessed"))
            total = sum(r["size"] for r in entries)
            for r in entries:
                if total <= self.max_cache_bytes:
                    break
                if r["pinned"] or (r["sha256"], r["extension"]) == (row["sha256"], ext):
                    continue
                try:
                    self.blob(self.cache, r["sha256"], r["extension"]).unlink(missing_ok=True)
                except OSError:
                    continue
                db.execute("DELETE FROM cache WHERE sha256=? AND extension=?",
                           (r["sha256"], r["extension"]))
                total -= r["size"]
            item["local_path"] = str(dest)
            return item

    def status(self):
        with self.connect() as db:
            return {
                "source": str(self.source), "archive": str(self.archive),
                "cache": str(self.cache), "offline": True,
                "files": db.execute("SELECT COUNT(*) FROM files WHERE missing=0").fetchone()[0],
                "revisions": db.execute("SELECT COUNT(*) FROM versions").fetchone()[0]
            }


def handler_for(store):
    class Handler(BaseHTTPRequestHandler):
        def reply(self, obj, status=200, kind="application/json"):
            data = (obj if isinstance(obj, bytes) else
                    obj.encode("utf-8") if isinstance(obj, str) else json.dumps(obj).encode("utf-8"))
            self.send_response(status)
            self.send_header("Content-Type", kind + "; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urlsplit(self.path)
            args = parse_qs(url.query)
            path = args.get("path", [""])[0]
            try:
                if url.path == "/health":
                    return self.reply({"status": "ok"})
                if url.path == "/v1/status":
                    return self.reply(store.status())
                if url.path == "/v1/assets":
                    return self.reply({"assets": store.assets()})
                if url.path == "/v1/versions":
                    return self.reply({"versions": store.versions(path)})
                if url.path == "/v1/resolve":
                    r = args.get("revision", [None])[0]
                    return self.reply(store.resolve(path, int(r) if r else None,
                                                    pin=args.get("pin", ["0"])[0] == "1"))
                if url.path == "/":
                    entries = store.assets()[:500]
                    rows = "".join(
                        f'<li><a href="/asset?path={quote(x["path"])}">{escape(x["path"])}</a>'
                        f' — v{x["revision"]}{" (removed from share)" if x["missing"] else ""}</li>'
                        for x in entries)
                    return self.reply('<!doctype html><html><meta charset="utf-8">'
                        '<title>Tourmaline Asset History</title><body style="font:16px system-ui;'
                        'max-width:850px;margin:3em auto"><h1>Tourmaline Asset History</h1>'
                        '<p>Offline revision archive. The source share is never modified.</p>'
                        f'<ul>{rows}</ul></body></html>', kind="text/html")
                if url.path == "/asset":
                    versions = store.versions(path)
                    rows = "".join(
                        f'<li>Revision {v["revision"]} — {v["captured_at"]} — '
                        f'<a href="/v1/resolve?path={quote(path)}&revision={v["revision"]}&pin=1">'
                        'Resolve into local cache</a></li>' for v in versions)
                    return self.reply('<!doctype html><html><meta charset="utf-8">'
                        '<body style="font:16px system-ui;max-width:850px;margin:3em auto">'
                        f'<h1>{escape(path)}</h1><a href="/">Back</a><ul>{rows}</ul></body></html>',
                        kind="text/html")
                return self.reply({"error": "not found"}, 404)
            except (ValueError, TypeError) as exc:
                return self.reply({"error": str(exc)}, 400)
            except FileNotFoundError as exc:
                return self.reply({"error": str(exc)}, 404)
            except OSError as exc:
                return self.reply({"error": str(exc)}, 500)

        def log_message(self, format, *args):
            pass
    return Handler


def serve(store, port=8765, poll=3.0):
    stop = threading.Event()
    def watch():
        while not stop.is_set():
            try:
                store.scan()
            except OSError as exc:
                print(f"Scan failed: {exc}", flush=True)
            stop.wait(max(0.5, poll))
    watcher = threading.Thread(target=watch, daemon=True)
    watcher.start()
    try:
        with ThreadingHTTPServer(("127.0.0.1", port), handler_for(store)) as server:
            print(f"Tourmaline asset history: http://127.0.0.1:{port}/", flush=True)
            server.serve_forever()
    finally:
        stop.set()
        watcher.join(timeout=5)


def main(argv=None):
    p = argparse.ArgumentParser(description="Offline Tourmaline source history service")
    p.add_argument("--config", required=True)
    sub = p.add_subparsers(dest="command")
    for name in ("serve", "scan", "status", "list"):
        sub.add_parser(name)
    v = sub.add_parser("versions")
    v.add_argument("path")
    r = sub.add_parser("resolve")
    r.add_argument("path")
    r.add_argument("--revision", type=int)
    r.add_argument("--pin", action="store_true")
    args = p.parse_args(argv)
    settings = json.loads(Path(args.config).read_text(encoding="utf-8"))
    if settings.get("schema_version") != 1:
        p.error("unsupported config version")
    store = Archive(settings["source"], settings["archive"], settings["cache"],
                    settle=settings.get("settle_seconds", 2),
                    max_cache_bytes=int(settings.get("cache_gib", 20) * 1024**3))
    if args.command in (None, "serve"):
        try:
            serve(store, int(settings.get("port", 8765)), float(settings.get("poll_seconds", 3)))
        except KeyboardInterrupt:
            return 0
    elif args.command == "scan":
        print(json.dumps(store.scan(), indent=2))
    elif args.command == "status":
        print(json.dumps(store.status(), indent=2))
    elif args.command == "list":
        print(json.dumps(store.assets(), indent=2))
    elif args.command == "versions":
        print(json.dumps(store.versions(args.path), indent=2))
    elif args.command == "resolve":
        print(json.dumps(store.resolve(args.path, args.revision, pin=args.pin), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
