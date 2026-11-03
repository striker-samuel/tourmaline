"""Workstation-side immutable cache for the Tourmaline shared repository.

The NAS server exposes metadata and bytes, never workstation-local paths.
All downloads are SHA-256 verified and reused from an on-disk SQLite index.
Requires only Python's standard library, including inside Autodesk Maya.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sqlite3
import tempfile
import time
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen


_HEX = re.compile(r"[0-9a-f]{64}\Z")
_ALLOWED_HOST = re.compile(r"[A-Za-z0-9][A-Za-z0-9.-]*\Z")


def default_cache():
    base = (os.environ.get("LOCALAPPDATA")
            if os.name == "nt" else os.environ.get("XDG_CACHE_HOME"))
    if not base:
        base = str(Path.home() / ("AppData/Local" if os.name == "nt" else ".cache"))
    return Path(base) / "Tourmaline" / "SharedAssetCache"


class AssetClient:
    def __init__(self, url, token, cache=None, max_bytes=20 * 1024**3):
        value = url.rstrip("/")
        parts = urlsplit(value)
        if (parts.scheme not in ("http", "https") or not parts.hostname
                or not _ALLOWED_HOST.fullmatch(parts.hostname)
                or parts.username or parts.password or parts.path or parts.query
                or parts.fragment):
            raise ValueError("expected an http(s) asset repository URL with host and port")
        if not isinstance(token, str) or len(token) < 24:
            raise ValueError("a NAS access token is required")
        if max_bytes < 0:
            raise ValueError("max_bytes cannot be negative")
        self.url = value
        self.token = token
        self.cache = Path(cache or default_cache()).expanduser().resolve()
        self.cache.mkdir(parents=True, exist_ok=True)
        (self.cache / "blobs").mkdir(exist_ok=True)
        self.max_bytes = int(max_bytes)
        self.db_path = self.cache / "local_cache.sqlite3"
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS items(
                    sha256 TEXT NOT NULL,extension TEXT NOT NULL,
                    size INTEGER NOT NULL,last_access REAL NOT NULL,
                    pinned INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY(sha256,extension));
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.db_path, timeout=30)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA busy_timeout=30000")
            yield db
            db.commit()
        except BaseException:
            db.rollback()
            raise
        finally:
            db.close()

    def request(self, endpoint, params=None):
        url = self.url + endpoint
        if params:
            url += "?" + urlencode(params)
        return Request(url, headers={"Authorization": "Bearer " + self.token})

    def get(self, endpoint, **params):
        with urlopen(self.request(endpoint, params), timeout=30) as fd:
            return json.load(fd)

    def sources(self):
        return self.get("/v2/sources")["sources"]

    def revisions(self, source_asset_id):
        return self.get("/v2/sources/" + source_asset_id + "/revisions")["revisions"]

    def products(self, source_asset_id=None):
        return self.get("/v2/products", **(
            {"source_asset_id": source_asset_id} if source_asset_id else {}))["products"]

    @staticmethod
    def sha256(path):
        h = hashlib.sha256()
        with path.open("rb") as fd:
            for data in iter(lambda: fd.read(1024 * 1024), b""):
                h.update(data)
        return h.hexdigest()

    def _cached_file(self, sha, extension):
        return self.cache / "blobs" / sha[:2] / (sha + extension)

    def _evict(self, db, protected):
        rows = list(db.execute("SELECT * FROM items ORDER BY last_access"))
        total = sum(row["size"] for row in rows)
        for row in rows:
            if total <= self.max_bytes:
                break
            key = (row["sha256"], row["extension"])
            if row["pinned"] or key == protected:
                continue
            path = self._cached_file(*key)
            try:
                path.unlink(missing_ok=True)
            except OSError:
                continue
            db.execute("DELETE FROM items WHERE sha256=? AND extension=?", key)
            total -= row["size"]

    def fetch(self, sha, filename, size, pin=False):
        if not isinstance(sha, str) or not _HEX.fullmatch(sha):
            raise ValueError("invalid artifact digest")
        suffix = PurePosixPath(filename).suffix.lower()
        if not re.fullmatch(r"\.[a-z0-9]{1,16}", suffix or ""):
            suffix = ""
        dest = self._cached_file(sha, suffix)
        if not dest.is_file() or self.sha256(dest) != sha:
            dest.parent.mkdir(parents=True, exist_ok=True)
            fd, temp = tempfile.mkstemp(prefix=".download-", dir=dest.parent)
            actual = hashlib.sha256()
            received = 0
            try:
                with os.fdopen(fd, "wb") as out, urlopen(
                        self.request("/v2/blobs/" + sha), timeout=120) as source:
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        out.write(chunk)
                        actual.update(chunk)
                        received += len(chunk)
                    out.flush()
                    os.fsync(out.fileno())
                if received != size or actual.hexdigest() != sha:
                    raise OSError("downloaded artifact failed size/SHA-256 verification")
                os.replace(temp, dest)
            finally:
                Path(temp).unlink(missing_ok=True)

        with self.connect() as db:
            db.execute(
                "INSERT INTO items(sha256,extension,size,last_access,pinned) "
                "VALUES(?,?,?,?,?) ON CONFLICT(sha256,extension) DO UPDATE SET "
                "size=excluded.size,last_access=excluded.last_access,"
                "pinned=MAX(items.pinned,excluded.pinned)",
                (sha, suffix, size, time.time(), int(pin)))
            self._evict(db, (sha, suffix))
        return str(dest)

    def resolve_source(self, source_asset_id, revision=None, pin=True):
        args = {"asset_id": source_asset_id}
        if revision is not None:
            args["revision"] = int(revision)
        entry = self.get("/v2/resolve", **args)
        entry["local_path"] = self.fetch(
            entry["sha256"], entry["source_path"], entry["size"], pin=pin)
        return entry

    def resolve_path(self, path, revision=None, pin=True):
        entries = self.get("/v1/versions", path=path)["versions"]
        if revision is None:
            item = entries[0] if entries else None
        else:
            item = next((x for x in entries if x["revision"] == int(revision)), None)
        if item is None:
            raise FileNotFoundError("archived source revision is missing")
        item["local_path"] = self.fetch(
            item["sha256"], path, item["size"], pin=pin)
        return item

    def resolve_product(self, product_id, version=None, pin=True):
        entries = [x for x in self.products() if x["product_id"] == product_id]
        if version is None:
            item = max(entries, key=lambda x: x["version"]) if entries else None
        else:
            item = next((x for x in entries if x["version"] == int(version)), None)
        if item is None:
            raise FileNotFoundError("published product version is missing")
        item["local_path"] = self.fetch(
            item["artifact_sha256"], item["filename"], item["size"], pin=pin)
        return item
