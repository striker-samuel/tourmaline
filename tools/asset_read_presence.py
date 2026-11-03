"""Observed read presence for Tourmaline source assets and published products.

Advisory visibility ONLY. Presence is not a lease, lock, reservation, checkout,
permission mechanism, or guarantee that an external SMB file handle is open.
Clients explicitly report complete sets of versions they are using. Sessions
expire when heartbeats stop; the writer/scanner is never blocked.
"""
from __future__ import annotations

from collections import defaultdict
import time
import uuid


_KINDS = frozenset(("maya", "editor", "agent", "worker", "tool", "other"))
_RESOURCE_KINDS = frozenset(("source", "product"))
_MAX_READS = 256


def canonical_uuid(raw):
    try:
        parsed = uuid.UUID(raw)
    except (TypeError, ValueError, AttributeError) as exc:
        raise ValueError("expected a canonical UUID") from exc
    value = str(parsed)
    if value != raw:
        raise ValueError("expected a canonical UUID")
    return value


class ConsumerPresence:
    def __init__(self, archive, ttl_seconds=90, clock=None):
        if not 15 <= float(ttl_seconds) <= 86400:
            raise ValueError("consumer presence TTL must be 15-86400 seconds")
        self.archive = archive
        self.ttl = float(ttl_seconds)
        self.clock = clock or time.time
        with archive.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS consumer_sessions (
                    session_id TEXT NOT NULL PRIMARY KEY,
                    label TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    last_seen REAL NOT NULL,
                    expires_at REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS consumer_reads (
                    session_id TEXT NOT NULL,
                    resource_kind TEXT NOT NULL,
                    resource_id TEXT NOT NULL,
                    resource_version INTEGER NOT NULL,
                    PRIMARY KEY(session_id, resource_kind, resource_id, resource_version));
                CREATE INDEX IF NOT EXISTS idx_consumer_reads_resource
                    ON consumer_reads(resource_kind, resource_id);
            """)

    def _purge(self, db, now):
        # SQLite foreign keys are not assumed to be enabled in legacy
        # archive connections. Remove children explicitly, atomically.
        expired = [r[0] for r in db.execute(
            "SELECT session_id FROM consumer_sessions WHERE expires_at<=?", (now,))]
        for session_id in expired:
            db.execute("DELETE FROM consumer_reads WHERE session_id=?", (session_id,))
            db.execute("DELETE FROM consumer_sessions WHERE session_id=?", (session_id,))

    def _normalize(self, reads):
        if not isinstance(reads, list) or len(reads) > _MAX_READS:
            raise ValueError("reads must be an array containing up to 256 entries")
        normalized = set()
        for item in reads:
            if not isinstance(item, dict):
                raise ValueError("each read must be an object")
            kind = item.get("kind")
            if kind not in _RESOURCE_KINDS:
                raise ValueError("read kind must be source or product")
            ident = canonical_uuid(item.get("id"))
            version = item.get("version")
            if type(version) is not int or version < 1:
                raise ValueError("read version must be a positive integer")
            normalized.add((kind, ident, version))
        return sorted(normalized)

    def heartbeat(self, session_id, label, kind, reads):
        session_id = canonical_uuid(session_id)
        if not isinstance(label, str) or not 1 <= len(label) <= 80 or any(
                ord(c) < 32 for c in label):
            raise ValueError("consumer label must be 1-80 printable characters")
        if kind not in _KINDS:
            raise ValueError("unknown consumer kind")
        normalized = self._normalize(reads)
        now = float(self.clock())
        with self.archive.connect() as db:
            self._purge(db, now)
            # Reject unrecognized resource IDs/versions rather than presenting
            # invented activity. This is informational, not a read permission.
            for res_kind, ident, version in normalized:
                if res_kind == "source":
                    found = db.execute(
                        "SELECT 1 FROM source_revisions WHERE asset_id=? AND revision=?",
                        (ident, version)).fetchone()
                else:
                    found = db.execute(
                        "SELECT 1 FROM published_versions WHERE product_id=? AND version=?",
                        (ident, version)).fetchone()
                if not found:
                    raise ValueError(
                        f"unknown {res_kind} version {ident} @ {version}")
            db.execute(
                "INSERT INTO consumer_sessions(session_id,label,kind,last_seen,expires_at)"
                " VALUES(?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET "
                "label=excluded.label,kind=excluded.kind,"
                "last_seen=excluded.last_seen,expires_at=excluded.expires_at",
                (session_id, label, kind, now, now + self.ttl))
            db.execute("DELETE FROM consumer_reads WHERE session_id=?", (session_id,))
            db.executemany(
                "INSERT INTO consumer_reads(session_id,resource_kind,resource_id,resource_version)"
                " VALUES(?,?,?,?)",
                ((session_id, key, ident, version)
                 for key, ident, version in normalized))
        return {"session_id": session_id, "last_seen": now,
                "expires_at": now + self.ttl, "reads": len(normalized)}

    def release(self, session_id):
        session_id = canonical_uuid(session_id)
        with self.archive.connect() as db:
            db.execute("DELETE FROM consumer_reads WHERE session_id=?", (session_id,))
            db.execute("DELETE FROM consumer_sessions WHERE session_id=?", (session_id,))
        return {"released": True, "session_id": session_id}

    def active(self, resource_kind=None, resource_id=None):
        if resource_kind is not None and resource_kind not in _RESOURCE_KINDS:
            raise ValueError("resource_kind must be source or product")
        if resource_id is not None:
            canonical_uuid(resource_id)
        if resource_id is not None and resource_kind is None:
            raise ValueError("resource_kind is required with resource_id")
        now = float(self.clock())
        with self.archive.connect() as db:
            self._purge(db, now)
            sessions = [dict(row) for row in db.execute(
                "SELECT session_id,label,kind,last_seen,expires_at "
                "FROM consumer_sessions ORDER BY label,session_id")]
            records = [dict(row) for row in db.execute(
                "SELECT session_id,resource_kind,resource_id,resource_version "
                "FROM consumer_reads ORDER BY resource_kind,resource_id,resource_version")]
            sources = {row["asset_id"]: row["current_path"] for row in db.execute(
                "SELECT asset_id,current_path FROM source_assets")}
            products = {row["product_id"]: dict(row) for row in db.execute(
                "SELECT product_id,source_asset_id,representation,variant "
                "FROM published_products")}
        grouped = defaultdict(list)
        for record in records:
            kind, ident = record["resource_kind"], record["resource_id"]
            if resource_kind is not None and kind != resource_kind:
                continue
            if resource_id is not None and ident != resource_id:
                continue
            entry = {
                "kind": kind,
                "id": ident,
                "version": record["resource_version"],
                "path": sources.get(ident) if kind == "source" else
                        sources.get(products.get(ident, {}).get("source_asset_id")),
            }
            if kind == "product":
                product = products.get(ident, {})
                entry["representation"] = product.get("representation")
                entry["variant"] = product.get("variant")
            grouped[record["session_id"]].append(entry)
        result = []
        for session in sessions:
            matched = grouped[session["session_id"]]
            if resource_kind is not None and not matched:
                continue
            session["reads"] = matched
            result.append(session)
        return {"consumers": result, "as_of": now,
                "ttl_seconds": self.ttl, "advisory_only": True}
