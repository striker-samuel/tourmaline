#!/usr/bin/env python3
"""Pack browser (PyQt6) for .pak files.

Reads the pak manifest/header (no engine build needed) to inspect pack
contents and their VFS relationship:

- Header (32 B): magic "SMP4", reserved, flags, tocOffset, tocCount.
- Blobs (16-byte aligned payloads).
- TOC: per-entry pathHash (FNV-1a64), kind, flags, offset, size, path.
- Footer (24 B): tocOffset, tocCount, crc32 (covers everything before it),
  magic "KPM4".

VFS relationship (mirrors Engine/Asset/pak.h + Engine/Vfs/vfs.h):
- Only Texture blobs are visible through PakBackend (Exists/TryRead/ListDir
  keyed by the ORIGINAL vfs path, returning the embedded .ktx2 bytes).
- Scene/Mesh/Material/Light/Gi blobs are cluster-internal, consumed via
  PakReader by the cluster deserializer - invisible to VFS file reads.
- OverlayVfs is first-match-wins over mounts ordered by priority; this
  browser lets you mount several paks + a loose dir and test lookups.

Usage:
    py tools/pak_browser.py [pak ...] [--cli-dump]
    py tools/pak_browser.py --cli-dump path/to.pak   # headless manifest dump
"""
from __future__ import annotations

import os
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Pure-python pak model (mirrors engine/src/Asset/pak.cpp)
# ---------------------------------------------------------------------------

KPAK_MAGIC = 0x34504D53  # "SMP4" LE (the magic IS the version: bump the
KPAK_FOOTER_MAGIC = 0x344D504B  # "KPM4" LE trailing char on breaking changes)
KHEADER_SIZE = 32
KFOOTER_SIZE = 24
KTOC_FIXED = 36  # hash u64, kind u32, flags u32, off u64, size u64, pathLen u32
KALIGN = 16

KINDS = {1: "Scene", 2: "Mesh", 3: "Material", 4: "Texture", 5: "Light", 6: "Gi", 7: "Camera"}
KIND_BY_NAME = {v: k for k, v in KINDS.items()}
FLAG_LZ4 = 1 << 0

# KTX2 identifier is 12 bytes: AB 4B 54 58 20 <ver:2> BB 0D 0A 1A 0A.
# The bundled libktx writes version "20" (older files use "11"), so the
# check accepts any version tag and reports it.
KTX2_PREFIX = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20])
KTX2_SUFFIX = bytes([0xBB, 0x0D, 0x0A, 0x1A, 0x0A])


def ktx2_magic_info(ktx: bytes) -> tuple[bool, str]:
    if len(ktx) < 12:
        return False, "(too short)"
    ok = ktx[:5] == KTX2_PREFIX and ktx[7:12] == KTX2_SUFFIX
    try:
        ver = ktx[5:7].decode("ascii")
    except UnicodeDecodeError:
        ver = ktx[5:7].hex()
    return ok, ver


def fnv1a64(s: str) -> int:
    h = 14695981039346656037
    for ch in s.encode("utf-8"):
        h ^= ch
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def vfs_normalize(path: str) -> str:
    """Mirror smvk::vfs::Normalize (incl. //flintbase/ scheme). '' = invalid."""
    if not path:
        return ""
    p = path.replace("\\", "/")
    # scheme?
    low = p.lower()
    if low.startswith("//flintbase/") or low == "//flintbase":
        cut = len("//flintbase/")
        rest = p[cut:] if len(p) >= cut else ""
        norm, ok = _norm_rel(rest)
        if not ok:
            return ""
        return "//flintbase/" if not norm else "//flintbase/" + norm
    pc.frameUniforms.Get()if len(p) >= 2 and p[1] == ":":
        return ""
    if p.startswith("/"):
        return ""
    norm, ok = _norm_rel(p)
    return norm if ok else ""


def _norm_rel(path: str):
    parts: list[str] = []
    for seg in path.split("/"):
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            if not parts:
                return "", False
            parts.pop()
            continue
        parts.append(seg)
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
    blob_overruns_toc: bool = False

    @property
    def compressed(self) -> bool:
        return bool(self.flags & FLAG_LZ4)

    @property
    def vfs_visible(self) -> bool:
        # Mirrors PakBackend: only Texture blobs served via VFS.
        return self.kind == 4


@dataclass
class PakFile:
    source: str
    data: bytes = field(repr=False)
    version: int = 0
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
        return self.data[e.offset : e.offset + e.size]

    def payload(self, e: PakEntry) -> tuple[bytes, str]:
        """Return (bytes, note). Decompresses LZ4 transparently like PakReader."""
        stored = self.stored_payload(e)
        if not (e.flags & FLAG_LZ4):
            return stored, ""
        if len(stored) < 8:
            return stored, "LZ4 flag set but stored blob < 8 bytes"
        (uncompressed_size,) = struct.unpack_from("<Q", stored, 0)
        block = stored[8:]
        try:
            import lz4.block  # type: ignore

            raw = lz4.block.decompress(block, uncompressed_size=uncompressed_size)
            return bytes(raw), f"lz4: {len(stored)} stored -> {len(raw)} bytes"
        except ImportError:
            return stored, (
                f"LZ4-compressed (needs 'pip install lz4'): "
                f"stored {len(stored)} B, uncompressed {uncompressed_size} B"
            )
        except Exception as ex:  # corrupt block
            return stored, f"LZ4 decompress failed: {ex}"


def parse_pak(source: str, data: bytes) -> PakFile:
    pak = PakFile(source=source, data=data)
    if len(data) < KHEADER_SIZE + KFOOTER_SIZE:
        pak.error = f"too small ({len(data)} B < {KHEADER_SIZE + KFOOTER_SIZE})"
        return pak
    try:
        magic, ver, flags, toc_off, toc_count, reserved = struct.unpack_from(
            "<IIIQQI", data, 0
        )
    except struct.error as ex:
        pak.error = f"header unpack failed: {ex}"
        return pak
    pak.version, pak.flags = ver, flags
    pak.toc_offset, pak.toc_count = toc_off, toc_count
    pak.header_reserved = reserved
    if magic != KPAK_MAGIC:
        pak.error = f"bad magic 0x{magic:08X} (want 0x{KPAK_MAGIC:08X} 'SMP4' — recook from zero)"
        return pak
    foff = len(data) - KFOOTER_SIZE
    f_toc_off, f_toc_count, f_crc, f_magic = struct.unpack_from("<QQII", data, foff)
    pak.footer_toc_offset, pak.footer_toc_count = f_toc_off, f_toc_count
    pak.footer_crc_stored = f_crc
    pak.footer_crc_computed = zlib.crc32(data[: foff + 16]) & 0xFFFFFFFF
    pak.crc_ok = pak.footer_crc_computed == f_crc
    pak.header_footer_match = (f_toc_off == toc_off) and (f_toc_count == toc_count)
    if f_magic != KPAK_FOOTER_MAGIC:
        pak.error = f"bad footer magic 0x{f_magic:08X} (want 'KPM4')"
        return pak
    if not pak.header_footer_match:
        pak.error = (
            f"header/footer TOC mismatch (hdr {toc_off}/{toc_count} vs "
            f"ftr {f_toc_off}/{f_toc_count})"
        )
        return pak
    if not pak.crc_ok:
        pak.error = (
            f"CRC mismatch: stored 0x{f_crc:08X} != "
            f"computed 0x{pak.footer_crc_computed:08X}"
        )
        return pak
    # TOC
    off = toc_off
    for i in range(toc_count):
        if off + KTOC_FIXED > len(data):
            pak.error = f"TOC entry {i} overruns image"
            return pak
        h, kind, fl, b_off, b_sz, plen = struct.unpack_from("<QIIQQI", data, off)
        off += KTOC_FIXED
        if plen > 4096 or off + plen > len(data):
            pak.error = f"TOC entry {i}: path overruns image (len {plen})"
            return pak
        try:
            path = data[off : off + plen].decode("utf-8")
        except UnicodeDecodeError:
            pak.error = f"TOC entry {i}: path not valid UTF-8"
            return pak
        off += plen
        overruns = (b_off + b_sz) > toc_off
        entry = PakEntry(
            path_hash=h,
            kind=kind,
            kind_name=KINDS.get(kind, f"Unknown({kind})"),
            flags=fl,
            offset=b_off,
            size=b_sz,
            path=path,
            hash_ok=(h == fnv1a64(path)),
            blob_overruns_toc=overruns,
        )
        if fl & ~FLAG_LZ4:
            pak.error = f"entry '{path}': unknown flags 0x{fl:X}"
            return pak
        if (fl & FLAG_LZ4) and ver < 3:
            pak.error = f"entry '{path}': LZ4 flag in v{ver} pak"
            return pak
        if overruns:
            pak.error = f"blob overruns TOC: {path}"
            return pak
        if not entry.hash_ok:
            pak.error = f"path hash mismatch: {path}"
            return pak
        pak.entries.append(entry)
    return pak


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
    c = _Cur(payload)
    uri = c.string()
    mag = c.i32()
    mn = c.i32()
    ws = c.i32()
    wt = c.i32()
    ks = c.u64()
    info: dict = {"uri": uri, "mag": mag, "min": mn, "wrapS": ws, "wrapT": wt}
    if not c.ok:
        return {"error": "truncated texture header", **info}
    if c.o + ks != len(payload):
        return {"error": f"ktxSize {ks} != remaining {len(payload) - c.o}", **info}
    ktx = payload[c.o : c.o + ks]
    info["ktxBytes"] = ks
    magic_ok, magic_ver = ktx2_magic_info(ktx)
    info["ktxMagicOk"] = magic_ok
    info["ktxVersion"] = magic_ver
    info["ktxFirst12"] = ktx[:12].hex(" ") if ktx else "(empty)"
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


def cli_dump(path: str) -> int:
    try:
        data = Path(path).read_bytes()
    except OSError as ex:
        print(f"error: cannot read {path}: {ex}")
        return 2
    pak = parse_pak(path, data)
    print(f"pak: {path} ({len(data)} B)")
    print(
        f"header: magic=SMP4 flags={pak.flags} "
        f"tocOffset={pak.toc_offset} tocCount={pak.toc_count}"
    )
    print(
        f"footer: tocOffset={pak.footer_toc_offset} tocCount={pak.footer_toc_count} "
        f"crc=0x{pak.footer_crc_stored:08X} computed=0x{pak.footer_crc_computed:08X} "
        f"{'OK' if pak.crc_ok else 'MISMATCH'}"
    )
    if pak.error:
        print(f"INVALID: {pak.error}")
        return 1
    from collections import Counter

    counts = Counter(e.kind_name for e in pak.entries)
    print(f"entries: {len(pak.entries)} " + str(dict(counts)))
    for e in pak.entries:
        comp = " lz4" if e.compressed else ""
        vis = "VFS" if e.vfs_visible else "internal"
        print(
            f"  [{e.kind_name:8}] {e.offset:10} +{e.size:<10} "
            f"hash=0x{e.path_hash:016X}{comp} {vis}  {e.path}"
        )
    print("note: only Texture blobs are VFS-visible (PakBackend); the rest are")
    print("      cluster-internal and load via PakReader, not VFS file reads.")
    return 0


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
        kind: str  # "pak" | "loose"
        priority: int
        pak: PakFile | None = None
        loose_dir: str = ""

    class PakBrowser(QMainWindow):
        def __init__(self, initial_paks: list[str] | None = None):
            super().__init__()
            self.setWindowTitle("Pack Browser - manifest/header + VFS inspector")
            self.resize(1280, 800)
            self.mounts: list[Mount] = []
            self._build_ui()
            for p in initial_paks or []:
                self.add_pak_mount(p)

        # -- layout ---------------------------------------------------
        def _build_ui(self):
            mono = QFont("Consolas", 9)
            mono.setStyleHint(QFont.StyleHint.Monospace)

            central = QWidget()
            self.setCentralWidget(central)
            root = QVBoxLayout(central)

            # toolbar row
            bar = QHBoxLayout()
            self.btn_open = QPushButton("Open pak...")
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
                ["all kinds", "Scene", "Mesh", "Material", "Texture", "Light", "Gi"]
            )
            filt_row.addWidget(self.search, 1)
            filt_row.addWidget(self.kind_combo)
            left_l.addLayout(filt_row)

            self.table = QTableWidget(0, 7)
            self.table.setHorizontalHeaderLabels(
                ["path", "kind", "offset", "size", "flags", "hash", "VFS"]
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
                "PakBackend serves <b>Texture blobs only</b> through VFS "
                "(original path -> embedded .ktx2). Scene/Mesh/Material/Light/Gi "
                "are cluster-internal (PakReader, not VFS reads). "
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
            vfs_l.addWidget(QLabel("VFS tree (Texture paths, union over mounts)"))
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
        def add_pak_mount(self, path: str):
            try:
                data = Path(path).read_bytes()
            except OSError as ex:
                QMessageBox.warning(self, "Open failed", f"{path}:\n{ex}")
                return
            pak = parse_pak(path, data)
            prio = len(self.mounts)
            name = f"{Path(path).name} (pak p{prio})"
            self.mounts.append(Mount(name, "pak", prio, pak))
            if pak.error:
                QMessageBox.warning(
                    self, "Invalid pak", f"{path}:\n{pak.error}"
                )
            self.refresh_mount_list()
            self.mount_list.setCurrentRow(len(self.mounts) - 1)

        def on_open(self):
            paths, _ = QFileDialog.getOpenFileNames(
                self, "Open .pak", "", "Pak files (*.pak);;All files (*)"
            )
            for p in paths:
                self.add_pak_mount(p)

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
            pak = m.pak
            assert pak is not None
            rows = [
                ("file", f"{pak.source} ({pak.size} B)"),
                (
                    "magic",
                    f"SMP4 {'OK' if not pak.error else 'INVALID'} "
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
                    f"{sum(1 for e in pak.entries if e.vfs_visible)} Texture blobs",
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

        def filtered_entries(self) -> list[tuple[Mount | None, PakEntry]]:
            """Entries of the selected mount (pak) filtered by search/kind."""
            m = self.selected_mount()
            if m is None or m.kind != "pak" or not m.pak or m.pak.error:
                # invalid pak: still show nothing (error is in header pane)
                if m is not None and m.kind == "pak" and m.pak and m.pak.error:
                    return []
                return []
            q = self.search.text().strip().lower()
            kind = self.kind_combo.currentText()
            out = []
            for e in m.pak.entries:
                if kind != "all kinds" and e.kind_name != kind:
                    continue
                if q and q not in e.path.lower():
                    continue
                out.append((m, e))
            return out

        def refresh_table(self):
            rows = self.filtered_entries()
            self.table.setSortingEnabled(False)
            self.table.setRowCount(len(rows))
            for i, (_, e) in enumerate(rows):
                vals = [
                    e.path,
                    e.kind_name,
                    str(e.offset),
                    str(e.size),
                    ("lz4" if e.compressed else "none"),
                    f"0x{e.path_hash:016X}",
                    ("VFS" if e.vfs_visible else "internal"),
                ]
                for j, v in enumerate(vals):
                    it = QTableWidgetItem(v)
                    if j in (2, 3):
                        it.setData(Qt.ItemDataRole.DisplayRole, int(vals[j]))
                    self.table.setItem(i, j, it)
            self.table.setSortingEnabled(True)
            self.table.resizeColumnsToContents()
            if rows:
                self.table.selectRow(0)

        # -- detail ----------------------------------------------------
        def show_selected(self):
            rows = self.filtered_entries()
            sel = self.table.currentRow()
            if not (0 <= sel < len(rows)):
                # table may be sorted; fall back to path lookup
                items = self.table.selectedItems()
                if not items:
                    return
                path = self.table.item(items[0].row(), 0).text()
                m = self.selected_mount()
                if not m or m.kind != "pak" or not m.pak:
                    return
                e = next((x for x in m.pak.entries if x.path == path), None)
                if e is None:
                    return
                self.show_entry(m, e)
                return
            m, e = rows[sel]
            assert m is not None
            self.show_entry(m, e)

        def show_entry(self, m: Mount, e: PakEntry):
            assert m.pak is not None
            payload, note = m.pak.payload(e)
            lines = [
                f"path:      {e.path}",
                f"normalized:{vfs_normalize(e.path) or '(INVALID VFS PATH)'}",
                f"kind:      {e.kind_name} ({e.kind})",
                f"offset:    {e.offset}  size(stored): {e.size}  "
                f"size(payload): {len(payload)}",
                f"flags:     0x{e.flags:X} ({'lz4' if e.compressed else 'stored'})",
                f"hash:      0x{e.path_hash:016X} "
                f"({'OK' if e.hash_ok else 'MISMATCH'})",
                f"vfs:       {'VISIBLE via PakBackend (Texture)' if e.vfs_visible else 'internal-only (PakReader, not VFS reads)'}",
            ]
            if note:
                lines.append(f"codec:     {note}")
            # per-kind decode
            try:
                if e.kind == 4:
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
            """normalized vfs path -> provider mount names (Texture blobs + loose)."""
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
                        d = decode_texture(payload)
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
    args = [a for a in argv[1:] if not a.startswith("-")]
    if "--cli-dump" in argv[1:]:
        if not args:
            print("usage: pak_browser.py --cli-dump <pak> [more.pak ...]")
            return 2
        rc = 0
        for p in args:
            rc |= cli_dump(p)
        return rc
    if "--help" in argv[1:] or "-h" in argv[1:]:
        print(__doc__)
        return 0
    # --cli-validate: exit-code only check (CI friendly)
    if "--cli-validate" in argv[1:]:
        rc = 0
        for p in args:
            try:
                pak = parse_pak(p, Path(p).read_bytes())
            except OSError as ex:
                print(f"{p}: unreadable: {ex}")
                rc = 1
                continue
            print(f"{p}: {'OK' if not pak.error else 'INVALID: ' + pak.error}")
            rc |= 1 if pak.error else 0
        return rc
    return run_gui(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
