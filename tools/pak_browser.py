#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

"""Tourmaline package browser for SMP8 .pak archives and appmanifest 2 .tapp files.

GUI: python tools/pak_browser.py [file.pak | project.tapp ...]
CLI: python tools/pak_browser.py --cli-dump project.tapp
     python tools/pak_browser.py --cli-validate project.tapp

The .tapp is a release pointer (entry scene and ordered package names);
asset identities live in each package's #ids blob. SMP8 verifies the TOC at
open and verifies each stored payload independently on demand. CLI validation
checks every page as well as the metadata, without requiring PyQt6 or lz4.
"""
from __future__ import annotations

import os
import shlex
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

# See Engine/Asset/pak.h, appmanifest.h and docs/pak_vnext.md.
KPAK_MAGIC = 0x38504D53  # SMP8
KPAK_FOOTER_MAGIC = 0x384D504B  # KPM8
KHEADER_SIZE = 32
KFOOTER_SIZE = 24
KTOC_FIXED = 44  # hash, kind, flags, offset, size, slot, contentCrc, pathLen
KALIGN = 16
FLAG_LZ4 = 1

KINDS = {
    1: "Scene", 2: "Mesh", 3: "Material", 4: "Texture",
    5: "Light", 6: "Gi", 7: "Camera", 8: "Decal",
    9: "Terrain", 10: "Level", 11: "Sound", 12: "Shader",
    13: "Plan", 14: "Schema", 15: "Identity", 16: "AudioGraph",
}
KIND_BY_NAME = {name: kind for kind, name in KINDS.items()}
VFS_KINDS = frozenset((4, 11, 12, 16))
# The catalog kind ABI (PR #64): byte 0 is Nil — the invalid/unset
# kind, never stored; the real kinds start at 1. Kept in lockstep with
# the engine's kCatalogKind* constants (engine/include/Engine/Asset/
# appmanifest.h); new kinds APPEND here too.
CATALOG_KINDS = (
    "nil",
    "scene",
    "texture",
    "mesh",
    "sound",
    "shader",
    "gi",
    "audiograph",
    "spriteset",
    "statemachine",
    "navmesh",
)

KTX2_PREFIX = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20])
KTX2_SUFFIX = bytes([0xBB, 0x0D, 0x0A, 0x1A, 0x0A])


def ktx2_magic_info(ktx: bytes) -> tuple[bool, str]:
    if len(ktx) < 12:
        return False, "(too short)"
    ok = ktx[:5] == KTX2_PREFIX and ktx[7:12] == KTX2_SUFFIX
    try:
        version = ktx[5:7].decode("ascii")
    except UnicodeDecodeError:
        version = ktx[5:7].hex()
    return ok, version


def fnv1a64(s: str) -> int:
    h = 14695981039346656037
    for ch in s.encode("utf-8"):
        h ^= ch
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def vfs_normalize(path: str) -> str:
    """Mirror smvk::vfs::Normalize, including the //flintbase/ scheme."""
    if not path:
        return ""
    p = path.replace("\\", "/")
    low = p.lower()
    if low == "//flintbase" or low.startswith("//flintbase/"):
        rest = p[len("//flintbase/"):] if low.startswith("//flintbase/") else ""
        normalized, ok = _norm_rel(rest)
        return ("//flintbase/" + normalized) if ok else ""
    if p.startswith("/") or (len(p) >= 2 and p[1] == ":"):
        return ""
    normalized, ok = _norm_rel(p)
    return normalized if ok else ""


def _norm_rel(path: str) -> tuple[str, bool]:
    parts: list[str] = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                return "", False
            parts.pop()
        else:
            parts.append(part)
    return "/".join(parts), True


@dataclass
class PakEntry:
    path_hash: int
    kind: int
    kind_name: str
    flags: int
    offset: int
    size: int
    path: str
    hash_ok: bool
    slot: int = 0
    content_crc: int = 0
    blob_overruns_toc: bool = False
    page_crc_ok: bool | None = None

    @property
    def compressed(self) -> bool:
        return bool(self.flags & FLAG_LZ4)

    @property
    def vfs_visible(self) -> bool:
        return self.kind in VFS_KINDS


@dataclass
class PakFile:
    source: str
    data: bytes = field(repr=False)
    version: int = 8
    flags: int = 0
    toc_offset: int = 0
    toc_count: int = 0
    header_reserved: int = 0
    footer_toc_offset: int = 0
    footer_toc_count: int = 0
    footer_crc_stored: int = 0
    footer_crc_computed: int = 0
    crc_ok: bool = False
    header_footer_match: bool = False
    error: str = ""
    entries: list[PakEntry] = field(default_factory=list)

    @property
    def valid(self) -> bool:
        return not self.error

    @property
    def size(self) -> int:
        return len(self.data)

    def stored_payload(self, e: PakEntry) -> bytes:
        return self.data[e.offset:e.offset + e.size]

    def verify_page(self, e: PakEntry) -> bool:
        if e.page_crc_ok is None:
            e.page_crc_ok = (
                zlib.crc32(self.stored_payload(e)) & 0xFFFFFFFF
            ) == e.content_crc
        return e.page_crc_ok

    def verify_all(self) -> list[str]:
        if self.error:
            return [self.error]
        return [
            f"{e.path}: content CRC mismatch (slot {e.slot})"
            for e in self.entries if not self.verify_page(e)
        ]

    def payload(self, e: PakEntry) -> tuple[bytes, str]:
        """Verify stored page before decompressing; never decode a corrupt page."""
        if not self.verify_page(e):
            return b"", (
                f"CONTENT CRC MISMATCH: stored=0x{e.content_crc:08X} "
                f"computed=0x{zlib.crc32(self.stored_payload(e)) & 0xFFFFFFFF:08X}"
            )
        stored = self.stored_payload(e)
        if not e.compressed:
            return stored, ""
        if len(stored) < 8:
            return b"", "invalid LZ4 wrapper (less than 8 bytes)"
        (size,) = struct.unpack_from("<Q", stored)
        try:
            import lz4.block  # type: ignore
            raw = lz4.block.decompress(stored[8:], uncompressed_size=size)
            return bytes(raw), f"lz4: {len(stored)} stored -> {len(raw)} bytes"
        except ImportError:
            return b"", f"LZ4 page needs 'pip install lz4' ({size} uncompressed bytes)"
        except Exception as exc:
            return b"", f"LZ4 decompression failed: {exc}"


def parse_pak(source: str, data: bytes) -> PakFile:
    pak = PakFile(source=source, data=data)
    if len(data) < KHEADER_SIZE + KFOOTER_SIZE:
        pak.error = "pak too small"
        return pak
    magic, reserved, flags, toc_off, toc_count, reserved2 = struct.unpack_from(
        "<IIIQQI", data
    )
    pak.flags, pak.toc_offset, pak.toc_count = flags, toc_off, toc_count
    pak.header_reserved = reserved
    if magic != KPAK_MAGIC:
        pak.error = f"bad magic 0x{magic:08X} (expected SMP8; recook stale packages)"
        return pak
    foff = len(data) - KFOOTER_SIZE
    f_toc_off, f_toc_count, f_crc, f_magic = struct.unpack_from(
        "<QQII", data, foff
    )
    pak.footer_toc_offset, pak.footer_toc_count = f_toc_off, f_toc_count
    pak.footer_crc_stored = f_crc
    pak.header_footer_match = (f_toc_off, f_toc_count) == (toc_off, toc_count)
    if f_magic != KPAK_FOOTER_MAGIC:
        pak.error = "bad footer magic (expected KPM8)"
        return pak
    if not pak.header_footer_match:
        pak.error = "header/footer TOC mismatch"
        return pak
    if not KHEADER_SIZE <= toc_off <= foff:
        pak.error = "TOC offset out of range"
        return pak
    toc = data[toc_off:foff]
    pak.footer_crc_computed = zlib.crc32(toc) & 0xFFFFFFFF
    pak.crc_ok = pak.footer_crc_computed == f_crc
    if not pak.crc_ok:
        pak.error = "TOC CRC mismatch"
        return pak
    # A large untrusted count must not preallocate or loop without bounds.
    if toc_count > len(toc) // KTOC_FIXED:
        pak.error = "TOC count exceeds available metadata"
        return pak
    off = 0
    for i in range(toc_count):
        if len(toc) - off < KTOC_FIXED:
            pak.error = f"TOC entry {i} truncated"
            return pak
        h, kind, fl, b_off, b_sz, slot, page_crc, plen = struct.unpack_from(
            "<QIIQQIII", toc, off
        )
        off += KTOC_FIXED
        if plen > 4096 or plen > len(toc) - off:
            pak.error = f"TOC entry {i}: invalid path length"
            return pak
        try:
            path = toc[off:off + plen].decode("utf-8")
        except UnicodeDecodeError:
            pak.error = f"TOC entry {i}: path is not UTF-8"
            return pak
        off += plen
        if b_off < KHEADER_SIZE or b_off > toc_off or b_sz > toc_off - b_off:
            pak.error = f"blob overruns TOC: {path}"
            return pak
        if fl & ~FLAG_LZ4:
            pak.error = f"unknown flags 0x{fl:X}: {path}"
            return pak
        if h != fnv1a64(path):
            pak.error = f"path hash mismatch: {path}"
            return pak
        pak.entries.append(PakEntry(
            path_hash=h, kind=kind, kind_name=KINDS.get(kind, f"Unknown({kind})"),
            flags=fl, offset=b_off, size=b_sz, path=path, hash_ok=True,
            slot=slot, content_crc=page_crc,
        ))
    if off != len(toc):
        pak.error = "TOC contains trailing bytes"
    return pak


@dataclass
class TappFile:
    source: str
    version: int = 0
    entry_pak: str = ""
    entry_scene: str = ""
    paks: list[str] = field(default_factory=list)
    error: str = ""

    @property
    def valid(self) -> bool:
        return not self.error


def _manifest_path_ok(path: str) -> bool:
    normalized = path.replace("\\", "/")
    parts = normalized.split("/")
    return bool(normalized) and not normalized.startswith("/") and not (
        len(normalized) >= 2 and normalized[1] == ":"
    ) and all(
        part not in ("", ".", "..") and "*" not in part and "?" not in part
        for part in parts
    )


def parse_tapp(source: str, text: str) -> TappFile:
    """Parse appmanifest 2, preserving the [paks] order and quoted names."""
    m = TappFile(source)
    saw_version = saw_entry = in_paks = False
    for number, raw in enumerate(text.lstrip("\ufeff").splitlines(), 1):
        try:
            tokens = shlex.split(raw, comments=True, posix=True)
        except ValueError as exc:
            m.error = f"line {number}: {exc}"
            return m
        if not tokens:
            continue
        if tokens == ["[paks]"]:
            in_paks = True
            continue
        if tokens[0].startswith("["):
            m.error = f"line {number}: unknown section {tokens[0]}"
            return m
        if in_paks:
            m.paks.append(" ".join(tokens))
        elif tokens[0] == "appmanifest" and len(tokens) == 2 and not saw_version:
            saw_version = True
            if not tokens[1].isdigit() or len(tokens[1]) > 4:
                m.error = f"line {number}: invalid appmanifest version"
                return m
            m.version = int(tokens[1])
        elif tokens[0] == "entry" and len(tokens) == 3 and not saw_entry:
            saw_entry = True
            m.entry_pak, m.entry_scene = tokens[1:]
        else:
            m.error = f"line {number}: invalid or duplicate directive {tokens[0]}"
            return m
    if not saw_version or m.version != 2:
        m.error = "requires appmanifest 2 (older manifests must be rebuilt)"
    elif not saw_entry or not m.entry_scene:
        m.error = "missing entry pak/scene"
    elif not m.paks:
        m.error = "manifest lists no paks"
    elif any(not _manifest_path_ok(p) for p in m.paks):
        m.error = "invalid manifest-relative pak path"
    elif len(m.paks) != len(set(m.paks)):
        m.error = "duplicate pak in manifest"
    elif m.entry_pak not in m.paks:
        m.error = f"entry pak {m.entry_pak!r} not listed in [paks]"
    return m


def tapp_pak_path(manifest: TappFile, name: str) -> Path:
    if not _manifest_path_ok(name):
        raise ValueError(f"invalid manifest pak path: {name}")
    return Path(manifest.source).parent.joinpath(*name.replace("\\", "/").split("/"))


def load_tapp(source: str) -> TappFile:
    return parse_tapp(source, Path(source).read_text(encoding="utf-8-sig"))


def decode_ids(payload: bytes) -> dict:
    """Decode the per-pak IDTR v1 catalog, not the removed .tapp [ids]."""
    if len(payload) < 12:
        return {"error": "truncated identity header"}
    magic, version, count = struct.unpack_from("<III", payload)
    if magic != 0x52544449 or version != 1 or count > (1 << 20):
        return {"error": "invalid IDTR magic, version, or row count"}
    off = 12
    rows = []
    try:
        for _ in range(count):
            if len(payload) - off < 13:
                raise ValueError("truncated identity row")
            asset_id, kind, path_len = struct.unpack_from("<QBI", payload, off)
            off += 13
            if not 0 < path_len <= 4096 or path_len > len(payload) - off:
                raise ValueError("invalid identity path length")
            path = payload[off:off + path_len].decode("utf-8")
            off += path_len
            if len(payload) - off < 4:
                raise ValueError("truncated scene name length")
            (name_len,) = struct.unpack_from("<I", payload, off)
            off += 4
            if name_len > 4096 or name_len > len(payload) - off:
                raise ValueError("invalid scene name length")
            name = payload[off:off + name_len].decode("utf-8")
            off += name_len
            # Row invariants (the engine's ParseAssetIdsBlob is the
            # mirror): a nonzero id, a non-Nil kind inside the ABI,
            # and the scene-name rule — names ride SCENE rows only
            # (kind 1 since the Nil-is-zero shift, PR #64).
            if not asset_id or kind < 1 or kind >= len(CATALOG_KINDS) or (name and kind != 1):
                raise ValueError("invalid identity row")
            rows.append({
                "id": f"0x{asset_id:X}", "kind": CATALOG_KINDS[kind],
                "path": path, "scene": name,
            })
    except (ValueError, UnicodeDecodeError) as exc:
        return {"error": str(exc)}
    if off != len(payload):
        return {"error": "trailing identity table bytes"}
    return {"count": count, "rows": rows}


def package_id_info(pak: PakFile) -> dict:
    e = next((e for e in pak.entries if e.kind == 15 and e.path == "#ids"), None)
    if e is None:
        return {"error": "missing #ids identity table (recook for appmanifest 2)"}
    payload, note = pak.payload(e)
    if note.startswith(("CONTENT CRC", "LZ4 page", "LZ4 decompression", "invalid LZ4")):
        return {"error": note}
    return decode_ids(payload)


# ---------------------------------------------------------------------------
# Payload decoders (best-effort, never raise)
# ---------------------------------------------------------------------------


class _Cur:
    def __init__(self, buf: bytes):
        self.b = buf
        self.o = 0
        self.ok = True

    def u32(self) -> int:
        if self.o + 4 > len(self.b):
            self.ok = False
            return 0
        (v,) = struct.unpack_from("<I", self.b, self.o)
        self.o += 4
        return v

    def i32(self) -> int:
        if self.o + 4 > len(self.b):
            self.ok = False
            return 0
        (v,) = struct.unpack_from("<i", self.b, self.o)
        self.o += 4
        return v

    def u64(self) -> int:
        if self.o + 8 > len(self.b):
            self.ok = False
            return 0
        (v,) = struct.unpack_from("<Q", self.b, self.o)
        self.o += 8
        return v

    def f32(self) -> float:
        if self.o + 4 > len(self.b):
            self.ok = False
            return 0.0
        (v,) = struct.unpack_from("<f", self.b, self.o)
        self.o += 4
        return v

    def string(self) -> str:
        n = self.u32()
        if not self.ok or n > 64 * 1024 or self.o + n > len(self.b):
            self.ok = False
            return ""
        try:
            s = self.b[self.o : self.o + n].decode("utf-8")
        except UnicodeDecodeError:
            self.ok = False
            return ""
        self.o += n
        return s


def decode_texture(payload: bytes) -> dict:
    """Read legacy and digest-bearing v4 texture envelopes (best effort)."""
    c = _Cur(payload)
    uri = c.string()
    mag, mn, ws, wt = c.i32(), c.i32(), c.i32(), c.i32()
    info = {"uri": uri, "mag": mag, "min": mn, "wrapS": ws, "wrapT": wt}
    if not c.ok:
        return {"error": "truncated texture envelope", **info}
    offset = c.o
    candidates: list[tuple[int, int, int, str]] = []
    if offset + 8 <= len(payload):
        (ks,) = struct.unpack_from("<Q", payload, offset)
        if ks > 0 and ks == len(payload) - offset - 8:
            candidates.append((offset + 8, ks, 0, "legacy"))
    if offset + 12 <= len(payload):
        (dlen,) = struct.unpack_from("<I", payload, offset)
        if dlen <= 1024 * 1024 and dlen <= len(payload) - offset - 12:
            end = offset + 4 + dlen
            (ks,) = struct.unpack_from("<Q", payload, end)
            if ks > 0 and ks == len(payload) - end - 8:
                if dlen == 0 or (dlen == 8 and
                        struct.unpack_from("<I", payload, offset + 4)[0] == 1):
                    candidates.append((end + 8, ks, dlen, "v4"))
    if not candidates:
        return {"error": "invalid texture envelope length/digest", **info}
    start, ks, dlen, fmt = candidates[-1]
    ktx = payload[start:start + ks]
    magic_ok, magic_version = ktx2_magic_info(ktx)
    info.update({
        "envelope": fmt, "digestBytes": dlen, "ktxBytes": ks,
        "ktxMagicOk": magic_ok, "ktxVersion": magic_version,
        "ktxFirst12": ktx[:12].hex(" "),
    })
    if dlen == 8:
        info["tailFirstMip"] = struct.unpack_from("<I", payload, offset + 8)[0]
    return info

def decode_mesh(payload: bytes) -> dict:
    c = _Cur(payload)
    uri = c.string()
    vc, ic, sc = c.u32(), c.u32(), c.u32()
    if not c.ok or vc == 0 or vc > 16 * 1024 * 1024 or ic > 64 * 1024 * 1024 or sc > 4096:
        return {"error": "truncated/suspicious mesh counts", "uri": uri}
    for _ in range(sc):
        c.u32()
        c.u32()
        c.i32()
    aabb = [c.f32() for _ in range(6)]
    sphere = c.f32()
    topo = c.u32()
    group, lod, cand = c.u32(), c.u32(), c.u32()
    serr = c.f32()
    c.f32()
    tris = c.u32()
    acmr = c.f32()
    lm_chunk = c.u32()
    lm_n = c.u32()
    if not c.ok:
        return {"error": "truncated mesh header", "uri": uri}
    return {
        "uri": uri,
        "verts": vc,
        "indices": ic,
        "surfaces": sc,
        "aabbMin": aabb[:3],
        "aabbMax": aabb[3:],
        "sphereR": sphere,
        "topology": topo,
        "lodGroup": group,
        "lodIndex": lod,
        "lodCandidates": cand,
        "simplifyErr": serr,
        "tris": tris,
        "acmr": acmr,
        "lmChunk": ("none" if lm_chunk == 0xFFFFFFFF else lm_chunk),
        "lmXforms": lm_n,
        "payloadBytes": len(payload),
    }


def decode_gi(payload: bytes) -> dict:
    c = _Cur(payload)
    n = c.u32()
    if not c.ok or n > 1024:
        return {"error": "bad gi registry count"}
    rows = []
    for _ in range(n):
        kind = c.u32()
        name = c.string()
        if not c.ok or kind not in (0, 1) or not name:
            return {"error": "bad gi registry entry"}
        rows.append({"kind": ("probes" if kind == 0 else "lightmap"), "name": name})
    if not c.ok or c.o != len(payload):
        return {"error": "gi registry trailing bytes mismatch", "rows": rows}
    return {"count": n, "rows": rows}


def decode_scene_summary(payload: bytes) -> dict:
    """Node count + name-table summary without building a cluster."""
    c = _Cur(payload)
    nn = c.u32()
    if not c.ok or nn == 0 or nn > 1024 * 1024:
        return {"error": "bad scene node count"}
    for _ in range(nn):  # hierarchy: 5 x i32
        for _k in range(5):
            c.i32()
    for _ in range(nn):  # transforms: 16 x f32
        for _k in range(16):
            c.f32()
    mesh_pairs = c.u32()
    for _ in range(mesh_pairs):
        c.u32()
        c.u32()
    mat_pairs = c.u32()
    for _ in range(mat_pairs):
        c.u32()
        c.u32()
    name_pairs = c.u32()
    for _ in range(name_pairs):
        c.u32()
        c.u32()
    node_names = [c.string() for _ in range(c.u32())]
    mat_names = [c.string() for _ in range(c.u32())]
    if not c.ok:
        return {"error": "truncated scene blob", "nodes": nn}
    out: dict = {
        "nodes": nn,
        "meshLinks": mesh_pairs,
        "materialLinks": mat_pairs,
        "nodeNames": len(node_names),
        "materialNames": len(mat_names),
        "sampleNodeNames": node_names[:12],
    }
    # Best-effort scene-name tail (v5 section sits after static/lm/light
    # sections; only report when the tail parses cleanly).
    return out


def hexdump(buf: bytes, limit: int = 512) -> str:
    lines = []
    for off in range(0, min(len(buf), limit), 16):
        chunk = buf[off : off + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{off:08x}  {hexs:<47}  {asc}")
    if len(buf) > limit:
        lines.append(f"... ({len(buf) - limit} more bytes)")
    return "\n".join(lines) if lines else "(empty)"


# ---------------------------------------------------------------------------
# CLI dump (headless manifest, same bytes the UI shows)
# ---------------------------------------------------------------------------


def _read_pak(path: str) -> PakFile:
    try:
        return parse_pak(path, Path(path).read_bytes())
    except OSError as exc:
        return PakFile(source=path, data=b"", error=f"cannot read: {exc}")


def _manifest_pak_problems(manifest: TappFile, name: str, pak: PakFile,
                           verify_pages: bool) -> list[str]:
    if pak.error:
        return [pak.error]
    issues = pak.verify_all() if verify_pages else []
    ids = package_id_info(pak)
    if "error" in ids:
        issues.append(ids["error"])
    elif name == manifest.entry_pak:
        scenes = [r for r in ids["rows"] if r["kind"] == "scene"]
        if not any(
            r["scene"] == manifest.entry_scene or
            (not r["scene"] and len(scenes) == 1)
            for r in scenes
        ):
            issues.append(f"entry scene {manifest.entry_scene!r} not found in #ids")
    return issues


def cli_dump(path: str, validate: bool = False) -> int:
    """Dump one pak or release pointer; validation checks all stored pages."""
    if path.lower().endswith(".tapp"):
        try:
            manifest = load_tapp(path)
        except (OSError, UnicodeError) as exc:
            print(f"{path}: cannot read manifest: {exc}")
            return 2
        print(f"tapp: {path}  appmanifest {manifest.version}")
        if manifest.error:
            print(f"INVALID: {manifest.error}")
            return 1
        print(f"entry: pak={manifest.entry_pak!r} scene={manifest.entry_scene!r}")
        print(f"paks: {len(manifest.paks)} (manifest order)")
        result = 0
        for name in manifest.paks:
            pak_path = str(tapp_pak_path(manifest, name))
            pak = _read_pak(pak_path)
            problems = _manifest_pak_problems(manifest, name, pak, validate)
            print(f"  {name}: {'INVALID: ' + '; '.join(problems) if problems else 'OK'}"
                  f" ({len(pak.entries)} entries)")
            if not validate and not pak.error:
                _print_pak_entries(pak, indent="    ")
                ids = package_id_info(pak)
                if "rows" in ids:
                    print(f"    #ids: {ids['count']} asset identities")
                    for row in ids["rows"]:
                        print(f"      {row['id']:>12} {row['kind']:<10} "
                              f"{row['path']} {row['scene']}")
            if problems:
                result = 1
        return result
    pak = _read_pak(path)
    print(f"pak: {path} ({pak.size} B)")
    print(f"header: SMP8 flags={pak.flags} tocOffset={pak.toc_offset} "
          f"tocCount={pak.toc_count}")
    print(f"footer: TOC CRC=0x{pak.footer_crc_stored:08X} "
          f"computed=0x{pak.footer_crc_computed:08X} "
          f"({'OK' if pak.crc_ok else 'MISMATCH'})")
    if pak.error:
        print(f"INVALID: {pak.error}")
        return 1
    problems = pak.verify_all() if validate else []
    if not validate:
        _print_pak_entries(pak)
        ids = package_id_info(pak) if any(e.path == "#ids" for e in pak.entries) else {}
        if "rows" in ids:
            print(f"#ids: {ids['count']} identities")
            for row in ids["rows"]:
                print(f"  {row['id']:>12} {row['kind']:<10} "
                      f"{row['path']} {row['scene']}")
    for problem in problems:
        print(f"INVALID: {problem}")
    if validate:
        print(f"{path}: {'OK (TOC + all pages)' if not problems else 'INVALID'}")
    return 1 if problems else 0


def _print_pak_entries(pak: PakFile, indent: str = "") -> None:
    from collections import Counter
    counts = Counter(e.kind_name for e in pak.entries)
    print(f"{indent}entries: {len(pak.entries)} {dict(counts)}")
    for e in pak.entries:
        codec = "lz4" if e.compressed else "raw"
        vfs = "VFS" if e.vfs_visible else "internal"
        print(f"{indent}  [{e.kind_name:10}] slot={e.slot:<6} "
              f"{e.offset:>10} +{e.size:<10} "
              f"CRC=0x{e.content_crc:08X} {codec} {vfs} {e.path}")


# ---------------------------------------------------------------------------
# PyQt6 UI
# ---------------------------------------------------------------------------

try:
    from PyQt6.QtCore import Qt
    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import (
        QApplication,
        QComboBox,
        QFileDialog,
        QFormLayout,
        QGroupBox,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QListWidget,
        QMainWindow,
        QMessageBox,
        QPushButton,
        QSplitter,
        QStatusBar,
        QTabWidget,
        QTableWidget,
        QTableWidgetItem,
        QTextBrowser,
        QTreeWidget,
        QTreeWidgetItem,
        QVBoxLayout,
        QWidget,
    )

    HAVE_QT = True
except ImportError:
    HAVE_QT = False


if HAVE_QT:

    @dataclass
    class Mount:
        name: str
        kind: str  # "pak" | "tapp" | "loose"
        priority: int
        pak: PakFile | None = None
        loose_dir: str = ""
        tapp: TappFile | None = None
        manifest_source: str = ""

    class PakBrowser(QMainWindow):
        def __init__(self, initial_paks: list[str] | None = None):
            super().__init__()
            self.setWindowTitle("Tourmaline Package Browser - SMP8 + .tapp")
            self.resize(1280, 800)
            self.mounts: list[Mount] = []
            self._build_ui()
            for p in initial_paks or []:
                self.add_source(p)

        # -- layout ---------------------------------------------------
        def _build_ui(self):
            mono = QFont("Consolas", 9)
            mono.setStyleHint(QFont.StyleHint.Monospace)

            central = QWidget()
            self.setCentralWidget(central)
            root = QVBoxLayout(central)

            # toolbar row
            bar = QHBoxLayout()
            self.btn_open = QPushButton("Open .pak / .tapp...")
            self.btn_loose = QPushButton("Mount loose dir...")
            self.btn_up = QPushButton("Priority up")
            self.btn_down = QPushButton("Priority down")
            self.btn_remove = QPushButton("Remove mount")
            for b in (
                self.btn_open,
                self.btn_loose,
                self.btn_up,
                self.btn_down,
                self.btn_remove,
            ):
                bar.addWidget(b)
            bar.addStretch(1)
            root.addLayout(bar)

            splitter = QSplitter(Qt.Orientation.Horizontal)
            root.addWidget(splitter, 1)

            # left: mounts + header + TOC
            left = QWidget()
            left_l = QVBoxLayout(left)
            left_l.addWidget(QLabel("Mounts (OverlayVfs order: lowest priority first)"))
            self.mount_list = QListWidget()
            self.mount_list.setMaximumHeight(110)
            left_l.addWidget(self.mount_list)

            self.header_box = QGroupBox("Header / manifest (selected mount)")
            self.header_form = QFormLayout()
            self.header_box.setLayout(self.header_form)
            left_l.addWidget(self.header_box)

            filt_row = QHBoxLayout()
            self.search = QLineEdit()
            self.search.setPlaceholderText("filter path substring...")
            self.kind_combo = QComboBox()
            self.kind_combo.addItems(
                ["all kinds", *KINDS.values()]
            )
            filt_row.addWidget(self.search, 1)
            filt_row.addWidget(self.kind_combo)
            left_l.addLayout(filt_row)

            self.table = QTableWidget(0, 9)
            self.table.setHorizontalHeaderLabels(
                ["path", "package", "kind", "slot", "offset", "size", "flags", "CRC32", "VFS"]
            )
            self.table.setSortingEnabled(True)
            self.table.setSelectionBehavior(
                QTableWidget.SelectionBehavior.SelectRows
            )
            self.table.horizontalHeader().setStretchLastSection(True)
            left_l.addWidget(self.table, 1)
            splitter.addWidget(left)

            # right: tabs
            self.tabs = QTabWidget()
            self.detail = QTextBrowser()
            self.detail.setFont(mono)
            self.tabs.addTab(self.detail, "Entry detail")
            self.hexview = QTextBrowser()
            self.hexview.setFont(mono)
            self.tabs.addTab(self.hexview, "Payload")
            splitter.addWidget(self.tabs)

            # VFS tab
            vfs_tab = QWidget()
            vfs_l = QVBoxLayout(vfs_tab)
            note = QLabel(
                "PakBackend serves <b>Texture, Sound, Shader, AudioGraph</b> through VFS "
                "(other kinds are package-internal). "
                
                "Overlay = first-match-wins by mount priority."
            )
            note.setWordWrap(True)
            vfs_l.addWidget(note)
            look_row = QHBoxLayout()
            self.vfs_input = QLineEdit()
            self.vfs_input.setPlaceholderText(
                "vfs path to resolve, e.g. textures/brick.ktx2"
            )
            self.vfs_btn = QPushButton("Resolve")
            look_row.addWidget(self.vfs_input, 1)
            look_row.addWidget(self.vfs_btn)
            vfs_l.addLayout(look_row)
            self.vfs_result = QTextBrowser()
            self.vfs_result.setFont(mono)
            self.vfs_result.setMaximumHeight(160)
            vfs_l.addWidget(self.vfs_result)
            dir_row = QHBoxLayout()
            self.dir_input = QLineEdit()
            self.dir_input.setPlaceholderText("vfs dir to list, e.g. textures (empty = root)")
            self.dir_btn = QPushButton("ListDir (union)")
            dir_row.addWidget(self.dir_input, 1)
            dir_row.addWidget(self.dir_btn)
            vfs_l.addLayout(dir_row)
            self.dir_result = QListWidget()
            vfs_l.addWidget(self.dir_result, 1)
            vfs_l.addWidget(QLabel("VFS tree (readable asset paths across mounts)"))
            self.tree = QTreeWidget()
            self.tree.setHeaderLabels(["vfs path", "providers"])
            vfs_l.addWidget(self.tree, 1)
            self.tabs.addTab(vfs_tab, "VFS")

            self.setStatusBar(QStatusBar())

            # signals
            self.btn_open.clicked.connect(self.on_open)
            self.btn_loose.clicked.connect(self.on_loose)
            self.btn_remove.clicked.connect(self.on_remove)
            self.btn_up.clicked.connect(lambda: self.move_mount(-1))
            self.btn_down.clicked.connect(lambda: self.move_mount(1))
            self.mount_list.currentRowChanged.connect(self.refresh_all)
            self.table.itemSelectionChanged.connect(self.show_selected)
            self.search.textChanged.connect(self.refresh_table)
            self.kind_combo.currentIndexChanged.connect(self.refresh_table)
            self.vfs_btn.clicked.connect(self.resolve_vfs)
            self.vfs_input.returnPressed.connect(self.resolve_vfs)
            self.dir_btn.clicked.connect(self.list_union_dir)
            self.dir_input.returnPressed.connect(self.list_union_dir)

        # -- mounts ---------------------------------------------------
        def add_source(self, path: str):
            if path.lower().endswith(".tapp"):
                self.add_tapp_mount(path)
            else:
                self.add_pak_mount(path)

        def add_pak_mount(self, path: str, manifest_source: str = "",
                          select: bool = True):
            pak = _read_pak(path)
            prio = len(self.mounts)
            self.mounts.append(Mount(
                f"{Path(path).name} (pak p{prio})", "pak", prio,
                pak=pak, manifest_source=manifest_source))
            if pak.error and not manifest_source:
                QMessageBox.warning(self, "Invalid pak", f"{path}:\n{pak.error}")
            if select:
                self.refresh_mount_list()
                self.mount_list.setCurrentRow(prio)

        def add_tapp_mount(self, path: str):
            try:
                manifest = load_tapp(path)
            except (OSError, UnicodeError) as exc:
                manifest = TappFile(source=path, error=str(exc))
            prio = len(self.mounts)
            self.mounts.append(Mount(
                f"{Path(path).name} (tapp)", "tapp", prio, tapp=manifest))
            if manifest.error:
                QMessageBox.warning(self, "Invalid .tapp", manifest.error)
            else:
                for name in manifest.paks:
                    self.add_pak_mount(
                        str(tapp_pak_path(manifest, name)),
                        manifest_source=path, select=False)
            self.refresh_mount_list()
            self.mount_list.setCurrentRow(prio)

        def on_open(self):
            paths, _ = QFileDialog.getOpenFileNames(
                self, "Open Tourmaline packages", "",
                "Tourmaline files (*.pak *.tapp);;All files (*)"
            )
            for path in paths:
                self.add_source(path)

        def on_loose(self):
            d = QFileDialog.getExistingDirectory(self, "Mount loose content dir")
            if not d:
                return
            prio = len(self.mounts)
            self.mounts.append(
                Mount(f"loose:{Path(d).name} (p{prio})", "loose", prio, None, d)
            )
            self.refresh_mount_list()
            self.mount_list.setCurrentRow(len(self.mounts) - 1)

        def on_remove(self):
            row = self.mount_list.currentRow()
            if 0 <= row < len(self.mounts):
                source = self.mounts[row].tapp.source if self.mounts[row].tapp else ""
                if source:
                    self.mounts = [
                        m for m in self.mounts
                        if m.manifest_source != source
                        and not (m.tapp and m.tapp.source == source)
                    ]
                else:
                    del self.mounts[row]
                self._renumber()
                self.refresh_mount_list()

        def move_mount(self, delta: int):
            row = self.mount_list.currentRow()
            j = row + delta
            if not (0 <= row < len(self.mounts) and 0 <= j < len(self.mounts)):
                return
            self.mounts[row], self.mounts[j] = self.mounts[j], self.mounts[row]
            self._renumber()
            self.refresh_mount_list()
            self.mount_list.setCurrentRow(j)

        def _renumber(self):
            for i, m in enumerate(self.mounts):
                m.priority = i

        def selected_mount(self) -> Mount | None:
            row = self.mount_list.currentRow()
            if 0 <= row < len(self.mounts):
                return self.mounts[row]
            return None

        def refresh_mount_list(self):
            self.mount_list.clear()
            for m in sorted(self.mounts, key=lambda x: x.priority):
                if m.kind == "pak":
                    n = len(m.pak.entries) if m.pak and not m.pak.error else 0
                    ok = "OK" if m.pak and not m.pak.error else "INVALID"
                    self.mount_list.addItem(f"[p{m.priority}] pak {m.name} - {n} entries ({ok})")
                elif m.kind == "tapp":
                    state = "INVALID" if m.tapp and m.tapp.error else "manifest"
                    self.mount_list.addItem(f"[p{m.priority}] tapp {m.name} ({state})")
                else:
                    self.mount_list.addItem(f"[p{m.priority}] loose {m.loose_dir}")
            self.refresh_all()

        # -- header / table -------------------------------------------
        def refresh_all(self):
            self.refresh_header()
            self.refresh_table()
            self.refresh_vfs_tree()

        def refresh_header(self):
            while self.header_form.rowCount():
                self.header_form.removeRow(0)
            m = self.selected_mount()
            if m is None:
                self.header_form.addRow(QLabel("(no mount selected)"))
                self.statusBar().showMessage("no mount")
                return
            if m.kind == "loose":
                self.header_form.addRow("type", QLabel("LooseBackend (host dir)"))
                self.header_form.addRow("root", QLabel(m.loose_dir))
                self.header_form.addRow(
                    "serves",
                    QLabel("all VFS paths (regular backends only)"),
                )
                self.statusBar().showMessage(f"loose {m.loose_dir}")
                return
            if m.kind == "tapp":
                manifest = m.tapp
                assert manifest is not None
                rows = [
                    ("file", manifest.source),
                    ("format", f"appmanifest {manifest.version}"),
                    ("entry", f"{manifest.entry_pak} / {manifest.entry_scene}"),
                    ("paks", f"{len(manifest.paks)} in manifest order"),
                ]
                for name in manifest.paks:
                    child = next((p for p in self.mounts if p.pak and
                                  p.manifest_source == manifest.source and
                                  p.pak.source == str(tapp_pak_path(manifest, name))), None)
                    rows.append((name, (child.pak.error or "OK") if child else "missing"))
                if manifest.error:
                    rows.append(("INVALID", manifest.error))
                for key, value in rows:
                    label = QLabel(value)
                    label.setWordWrap(True)
                    label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
                    self.header_form.addRow(key, label)
                return
            pak = m.pak
            assert pak is not None
            rows = [
                ("file", f"{pak.source} ({pak.size} B)"),
                (
                    "magic",
                    f"SMP8 {'OK' if not pak.error else 'INVALID'} "
                    f"(flags={pak.flags})".strip(),
                ),
                ("toc", f"offset={pak.toc_offset} count={pak.toc_count}"),
                (
                    "footer",
                    f"offset={pak.footer_toc_offset} count={pak.footer_toc_count} "
                    f"crc=0x{pak.footer_crc_stored:08X} "
                    f"({'OK' if pak.crc_ok else 'MISMATCH'})",
                ),
                ("entries", f"{len(pak.entries)}" + self._kind_counts(pak)),
                ("compressed", f"{sum(1 for e in pak.entries if e.compressed)} lz4"),
                (
                    "vfs-visible",
                    f"{sum(1 for e in pak.entries if e.vfs_visible)} assets",
                ),
            ]
            if pak.error:
                rows.append(("INVALID", pak.error))
            for k, v in rows:
                lab = QLabel(v)
                lab.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
                self.header_form.addRow(k, lab)
            self.statusBar().showMessage(
                f"{Path(pak.source).name}: {len(pak.entries)} entries, "
                f"crc {'OK' if pak.crc_ok else 'BAD'}"
            )

        @staticmethod
        def _kind_counts(pak: PakFile) -> str:
            from collections import Counter

            c = Counter(e.kind_name for e in pak.entries)
            return " (" + ", ".join(f"{k}:{v}" for k, v in sorted(c.items())) + ")"

        def filtered_entries(self):
            selected = self.selected_mount()
            if selected is None:
                return []
            if selected.kind == "tapp" and selected.tapp:
                sources = [m for m in self.mounts if m.kind == "pak"
                           and m.manifest_source == selected.tapp.source]
            elif selected.kind == "pak":
                sources = [selected]
            else:
                return []
            q = self.search.text().strip().lower()
            kind = self.kind_combo.currentText()
            return [(m, e) for m in sources if m.pak and not m.pak.error
                    for e in m.pak.entries
                    if (kind == "all kinds" or e.kind_name == kind)
                    and (not q or q in e.path.lower())]

        def refresh_table(self):
            rows = self.filtered_entries()
            self.table.setSortingEnabled(False)
            self.table.setRowCount(len(rows))
            for i, (mount, entry) in enumerate(rows):
                values = [entry.path, Path(mount.pak.source).name,
                          entry.kind_name, entry.slot, entry.offset, entry.size,
                          "lz4" if entry.compressed else "raw",
                          f"0x{entry.content_crc:08X}",
                          "VFS" if entry.vfs_visible else "internal"]
                for j, value in enumerate(values):
                    item = QTableWidgetItem()
                    item.setData(Qt.ItemDataRole.DisplayRole, value)
                    item.setData(Qt.ItemDataRole.UserRole, (mount, entry))
                    self.table.setItem(i, j, item)
            self.table.setSortingEnabled(True)
            self.table.resizeColumnsToContents()
            if rows:
                self.table.selectRow(0)

        def show_selected(self):
            items = self.table.selectedItems()
            if not items:
                return
            item = self.table.item(items[0].row(), 0)
            if item is not None:
                selected = item.data(Qt.ItemDataRole.UserRole)
                if selected:
                    self.show_entry(*selected)

        def show_entry(self, m: Mount, e: PakEntry):
            assert m.pak is not None
            payload, note = m.pak.payload(e)
            lines = [
                f"package:   {m.pak.source}",
                f"slot:      {e.slot}",
                f"contentCRC: 0x{e.content_crc:08X}",
                f"path:      {e.path}",
                f"normalized:{vfs_normalize(e.path) or '(INVALID VFS PATH)'}",
                f"kind:      {e.kind_name} ({e.kind})",
                f"offset:    {e.offset}  size(stored): {e.size}  "
                f"size(payload): {len(payload)}",
                f"flags:     0x{e.flags:X} ({'lz4' if e.compressed else 'stored'})",
                f"hash:      0x{e.path_hash:016X} "
                f"({'OK' if e.hash_ok else 'MISMATCH'})",
                f"vfs:       {'VISIBLE via PakBackend' if e.vfs_visible else 'internal-only (PakReader)'}",
            ]
            if note:
                lines.append(f"codec:     {note}")
            # per-kind decode
            try:
                if note and not note.startswith('lz4:'):
                    lines.append('payload unavailable; decode skipped')
                elif e.kind == 15:
                    lines.append('identity table:')
                    lines += self._fmt_dict(decode_ids(payload))
                elif e.kind == 4:
                    d = decode_texture(payload)
                    lines += self._fmt_texture(d)
                elif e.kind == 2:
                    d = decode_mesh(payload)
                    lines += self._fmt_dict(d)
                elif e.kind == 6:
                    d = decode_gi(payload)
                    lines.append("gi registry:")
                    lines += self._fmt_dict(d)
                elif e.kind == 1:
                    d = decode_scene_summary(payload)
                    lines.append("scene summary (no cluster build):")
                    lines += self._fmt_dict(d)
                else:
                    lines.append(f"(no structured decoder for {e.kind_name}; hex below)")
            except Exception as ex:
                lines.append(f"decode error: {ex}")
            # providers across mounts (collision diagnostics)
            provs = self.find_providers(e.path) if e.vfs_visible else []
            if e.vfs_visible:
                lines.append(
                    "providers: " + (", ".join(provs) if provs else "(none)")
                )
            self.detail.setPlainText("\n".join(lines))
            head = [f"; {e.path}  ({e.kind_name}, {len(payload)} B payload)"]
            self.hexview.setPlainText("\n".join(head) + "\n" + hexdump(payload))

        @staticmethod
        def _fmt_dict(d: dict) -> list[str]:
            out = []
            for k, v in d.items():
                if k == "rows" and isinstance(v, list):
                    for r in v[:50]:
                        out.append(f"  - {r}")
                    if len(v) > 50:
                        out.append(f"  ... ({len(v) - 50} more)")
                elif isinstance(v, list) and len(v) > 12:
                    out.append(f"  {k}: [{len(v)} items] {v[:12]}...")
                else:
                    out.append(f"  {k}: {v}")
            return out

        def _fmt_texture(self, d: dict) -> list[str]:
            out = ["texture payload (uri | samplers | ktxSize | ktx):"]
            out += self._fmt_dict(d)
            if d.get("ktxMagicOk") is False:
                out.append("  WARNING: embedded bytes are NOT KTX2 (bad magic)")
            return out

        # -- VFS --------------------------------------------------------
        def _texture_index(self) -> dict[str, list[str]]:
            """Normalized vfs path -> provider names (VFS-visible entries + loose)."""
            idx: dict[str, list[str]] = {}
            for m in sorted(self.mounts, key=lambda x: x.priority):
                if m.kind == "pak" and m.pak and not m.pak.error:
                    for e in m.pak.entries:
                        if e.vfs_visible:
                            idx.setdefault(e.path, []).append(m.name)
                elif m.kind == "loose" and os.path.isdir(m.loose_dir):
                    for root, _dirs, files in os.walk(m.loose_dir):
                        for f in files:
                            rel = os.path.relpath(os.path.join(root, f), m.loose_dir)
                            n = vfs_normalize(rel)
                            if n:
                                idx.setdefault(n, []).append(m.name)
            return idx

        def find_providers(self, vfs_path: str) -> list[str]:
            n = vfs_normalize(vfs_path)
            if not n:
                return []
            out = []
            for m in sorted(self.mounts, key=lambda x: x.priority):
                if m.kind == "pak" and m.pak and not m.pak.error:
                    if any(e.vfs_visible and e.path == n for e in m.pak.entries):
                        out.append(m.name)
                elif m.kind == "loose" and os.path.isdir(m.loose_dir):
                    if os.path.isfile(os.path.join(m.loose_dir, *n.split("/"))):
                        out.append(m.name)
            return out

        def resolve_vfs(self):
            raw = self.vfs_input.text()
            n = vfs_normalize(raw)
            if not n:
                self.vfs_result.setPlainText(f"'{raw}': INVALID vfs path (rejected by Normalize)")
                return
            provs = self.find_providers(n)
            lines = [f"input:      {raw}", f"normalized: {n}"]
            if not provs:
                mounts = ", ".join(m.name for m in self.mounts) or "(no mounts)"
                lines.append(f"NOT FOUND (mounts: {mounts})")
                self.vfs_result.setPlainText("\n".join(lines))
                return
            lines.append(f"served by:  {provs[0]}  (first-match-wins)")
            if len(provs) > 1:
                lines.append(f"shadowed:   {', '.join(provs[1:])} (legal overlap: loose overrides win by priority)")
            # size info from winner
            for m in sorted(self.mounts, key=lambda x: x.priority):
                if m.name != provs[0]:
                    continue
                if m.kind == "pak" and m.pak:
                    e = next(
                        (x for x in m.pak.entries if x.vfs_visible and x.path == n),
                        None,
                    )
                    if e is not None:
                        payload, _ = m.pak.payload(e)
                        d = decode_texture(payload) if e.kind == 4 else {}
                        lines.append(
                            f"blob:       [{e.kind_name}] offset={e.offset} stored={e.size} "
                            f"ktx={d.get('ktxBytes', '?')} B "
                            f"magicOk={d.get('ktxMagicOk', '?')}"
                        )
                else:
                    fp = os.path.join(m.loose_dir, *n.split("/"))
                    try:
                        lines.append(f"loose file: {fp} ({os.path.getsize(fp)} B)")
                    except OSError:
                        pass
                break
            self.vfs_result.setPlainText("\n".join(lines))

        def _listdir_one(self, m: Mount, ndir: str) -> list[str]:
            """PakBackend::ListDir semantics (Texture only) or host dir listing."""
            if m.kind == "pak":
                if not m.pak or m.pak.error:
                    return []
                out = set()
                prefix = "" if not ndir else ndir + "/"
                for e in m.pak.entries:
                    if not e.vfs_visible:
                        continue
                    p = e.path
                    if not ndir:
                        slash = p.find("/")
                        out.add(p if slash < 0 else p[:slash])
                    elif len(p) > len(prefix) and p.startswith(prefix):
                        rest = p[len(prefix):]
                        slash = rest.find("/")
                        out.add(p if slash < 0 else prefix + rest[:slash])
                return sorted(out)
            # loose
            base = m.loose_dir if not ndir else os.path.join(m.loose_dir, *ndir.split("/"))
            try:
                return sorted(
                    ("" if not ndir else ndir + "/") + x for x in os.listdir(base)
                )
            except OSError:
                return []

        def list_union_dir(self):
            raw = self.dir_input.text().strip()
            ndir = vfs_normalize(raw) if raw else ""
            if raw and not ndir and raw not in ("", "/", "."):
                self.dir_result.clear()
                self.dir_result.addItem(f"invalid dir '{raw}'")
                return
            union: dict[str, list[str]] = {}
            for m in sorted(self.mounts, key=lambda x: x.priority):
                for e in self._listdir_one(m, ndir):
                    union.setdefault(e, []).append(m.name)
            self.dir_result.clear()
            if not union:
                self.dir_result.addItem("(empty)")
                return
            for path in sorted(union):
                provs = union[path]
                tag = provs[0] + (f" (+{len(provs) - 1} shadowed)" if len(provs) > 1 else "")
                self.dir_result.addItem(f"{path}   [{tag}]")

        def refresh_vfs_tree(self):
            self.tree.clear()
            idx = self._texture_index()
            if not idx:
                return
            nodes: dict[str, QTreeWidgetItem] = {}
            for path in sorted(idx):
                parts = path.split("/")
                for depth in range(1, len(parts) + 1):
                    key = "/".join(parts[:depth])
                    if key in nodes:
                        continue
                    parent = nodes.get("/".join(parts[: depth - 1])) if depth > 1 else None
                    item = QTreeWidgetItem([parts[depth - 1], ""])
                    if parent is None:
                        self.tree.addTopLevelItem(item)
                    else:
                        parent.addChild(item)
                    nodes[key] = item
            for path, provs in idx.items():
                item = nodes.get(path)
                if item is not None:
                    item.setText(1, provs[0] + (f" +{len(provs)-1}" if len(provs) > 1 else ""))
            self.tree.expandToDepth(1)


def run_gui(initial: list[str]) -> int:
    if not HAVE_QT:
        print("error: PyQt6 not installed (pip install PyQt6)", file=sys.stderr)
        return 2
    plat = os.environ.get("QT_QPA_PLATFORM", "").strip().lower()
    if plat in ("offscreen", "minimal"):
        # These plugins have no real paint device: every widget repaint
        # fails with "QPainter: Painter not active" spam. Almost always
        # caused by a stale QT_QPA_PLATFORM left over from headless testing.
        print(
            f"error: QT_QPA_PLATFORM={os.environ.get('QT_QPA_PLATFORM')!r} has no "
            "paint device, so the interactive browser cannot render.\n"
            "Unset it and re-run from a desktop shell:\n"
            '  Remove-Item Env:\\QT_QPA_PLATFORM  # PowerShell\n'
            "  py tools/pak_browser.py <pak>...",
            file=sys.stderr,
        )
        return 2
    app = QApplication(sys.argv)
    win = PakBrowser(initial)
    win.show()
    return app.exec()


def main(argv: list[str]) -> int:
    if "--help" in argv[1:] or "-h" in argv[1:]:
        print(__doc__)
        return 0
    args = [a for a in argv[1:] if not a.startswith("-")]
    if "--cli-dump" in argv[1:] or "--cli-validate" in argv[1:]:
        if not args:
            print("usage: pak_browser.py (--cli-dump | --cli-validate) "
                  "<pak-or-tapp> [...]")
            return 2
        validate = "--cli-validate" in argv[1:]
        result = 0
        for path in args:
            result |= cli_dump(path, validate=validate)
        return result
    return run_gui(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
