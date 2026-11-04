#!/usr/bin/env python3
"""Detect text corruption from local edits (including PowerShell-based agents).

Run from any directory: python tools/check_text_integrity.py
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {
    ".bat", ".c", ".cc", ".cmake", ".cmd", ".cpp", ".cs", ".h",
    ".hlsl", ".hpp", ".ini", ".json", ".ps1", ".psd1", ".psm1",
    ".py", ".sh", ".toml", ".xml", ".yaml", ".yml", ".md", ".txt", ".rst", ".svg", ".html", ".css", ".js", ".ts", ".inl",
}
DASH_LOOKALIKES = frozenset("\u2010\u2011\u2012\u2013\u2014\u2015\u2212\ufe58\ufe63\uff0d")
MOJIBAKE = ("â€“", "â€”", "â€‘", "ï¿½")
# Self-referential exemption set: these files CONTAIN the corruption
# markers by design (the detector's own constants, its regression
# fixtures, and AGENTS.md's documentation of the failure symptoms) -
# without the exemption the scan fails on a clean tree and the Text
# integrity CI workflow is permanently red. Everything else stays
# checked.
EXEMPT_PATHS = frozenset((
    "tools/check_text_integrity.py",
    "tests/test_text_integrity.py",
    "AGENTS.md",
))


def check_text(path: Path, content: bytes) -> list[str]:
    """Return human-readable diagnostics; do not silently rewrite content."""
    try:
        text = content.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        return [f"{path}: invalid UTF-8 ({exc})"]

    errors = []
    # Do not ban genuine Unicode from source comments, strings or docs.
    # Flag damaged text and dash lookalikes in obvious command/option positions.
    is_powershell = path.suffix.lower() in {".ps1", ".psm1", ".psd1"}
    for number, line in enumerate(text.splitlines(), 1):
        if "\ufffd" in line:
            errors.append(f"{path}:{number}: Unicode replacement character U+FFFD")
        for marker in MOJIBAKE:
            if marker in line:
                errors.append(f"{path}:{number}: possible UTF-8 mojibake {marker!r}")
        # PowerShell comments may contain typographic punctuation. Script
        # content must retain ASCII hyphen-minus (U+002D) for command switches.
        if is_powershell and not line.lstrip().startswith("#"):
            for char in sorted(set(line) & DASH_LOOKALIKES):
                errors.append(
                    f"{path}:{number}: non-ASCII dash U+{ord(char):04X} "
                    "in PowerShell code (use ASCII '-' for switches)"
                )
    return errors


def tracked_files(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [
        Path(name.decode("utf-8", errors="surrogateescape"))
        for name in result.stdout.split(b"\0")
        if name
    ]


def scan(root: Path) -> tuple[int, list[str]]:
    count = 0
    errors = []
    for relative in tracked_files(root):
        if relative.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if relative.parts[0] == "external":
            continue  # Third-party sources are maintained upstream.
        if relative.as_posix() in EXEMPT_PATHS:
            continue  # Fixtures/documentation carry the markers by design.
        absolute = root / relative
        if not absolute.is_file():
            continue  # Submodules and missing sparse-checkout files.
        count += 1
        errors.extend(check_text(relative, absolute.read_bytes()))
    return count, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        count, errors = scan(args.root.resolve())
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"Text integrity scan failed: {exc}", file=sys.stderr)
        return 1
    for error in errors:
        print(error, file=sys.stderr)
    print(f"Checked {count} tracked source/config files: {len(errors)} issue(s).")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
