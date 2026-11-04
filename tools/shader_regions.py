#!/usr/bin/env python3
"""Discover individually addressable HLSL loops, branches and function sections.

Source-level discovery is deliberately conservative. IDs hash normalized
statement headers within the containing function; duplicate headers fail
closed instead of silently targeting the wrong loop after an AI rewrite.
SPIR-V reflection generally cannot reconstruct original HLSL statements.
"""
from __future__ import annotations

import difflib
import hashlib
import json
import re

from shader_hints import _code_mask

CONTROL = {"for", "while", "if", "switch", "catch"}
TOKEN = re.compile(r"\b(for|while|do|if)\b")
FUNCTION = re.compile(r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:\:\s*[^{};]+)?\s*$", re.S)


def _balanced_end(mask, start, opening, closing):
    if start >= len(mask) or mask[start] != opening:
        raise ValueError("expected " + opening)
    depth = 0
    for index in range(start, len(mask)):
        if mask[index] == opening:
            depth += 1
        elif mask[index] == closing:
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError("unbalanced shader syntax")


def _functions(mask):
    out = []
    for match in re.finditer(r"\{", mask):
        head = mask[max(0, match.start() - 512):match.start()]
        m = FUNCTION.search(head)
        if not m or m.group(1) in CONTROL:
            continue
        # Only accept declaration-like function headers (no statement
        # delimiters since the last closing brace / semicolon).
        name = m.group(1)
        try:
            end = _balanced_end(mask, match.start(), "{", "}")
        except ValueError:
            continue
        out.append({"name": name, "start": match.start(), "end": end})
    return out


def _normalized(value):
    return re.sub(r"\s+", "", value)


def discover(source):
    """Return source regions (IDs, coordinates, and editable function scope)."""
    mask = _code_mask(source)
    functions = _functions(mask)
    result = []
    for item in functions:
        start = item["start"]
        signature = mask[max(0, start - 256):start]
        # Disambiguate overloads via their normalized signatures.
        fingerprint = hashlib.sha256(_normalized(signature).encode()).hexdigest()[:12]
        result.append({"id": f"section:{item['name']}:{fingerprint}",
                       "kind": "section", "function": item["name"],
                       "start": item["start"], "end": item["end"],
                       "line": source.count("\n", 0, item["start"]) + 1})
    for token in TOKEN.finditer(mask):
        kind = token.group(1)
        pos = token.start()
        following = mask[token.end():]
        if kind != "do":
            open_pos = token.end() + len(following) - len(following.lstrip())
            if open_pos >= len(mask) or mask[open_pos] != "(":
                continue
            try:
                end = _balanced_end(mask, open_pos, "(", ")")
            except ValueError:
                continue
            header = mask[pos:end]
        else:
            header = "do"
        owner = next((f for f in reversed(functions) if f["start"] < pos < f["end"]), None)
        scope = owner["name"] if owner else "<global>"
        sig = _normalized(header)
        digest = hashlib.sha256((scope + ":" + kind + ":" + sig).encode()).hexdigest()[:12]
        region_kind = "loop" if kind in ("for", "while", "do") else "branch"
        result.append({"id": f"{region_kind}:{scope}:{digest}", "kind": region_kind,
                       "syntax": kind, "function": scope, "start": pos, "end": end if kind != "do" else pos + 2,
                       "line": source.count("\n", 0, pos) + 1,
                       "header": re.sub(r"\s+", " ", header).strip()[:180]})
    # Repeated indistinguishable statements are unsafe to edit by content ID.
    counts = {}
    for region in result:
        counts[region["id"]] = counts.get(region["id"], 0) + 1
    for region in result:
        region["ambiguous"] = counts[region["id"]] > 1
    return sorted(result, key=lambda r: (r["start"], r["kind"]))


def _line_positions(source):
    lines = source.splitlines(keepends=True)
    starts, offset = [], 0
    for line in lines:
        starts.append(offset)
        offset += len(line)
    return lines, starts


def _mapped_lines(previous, current):
    """Git-style correspondence of unchanged lines; changed hunks are unknown."""
    a, _ = _line_positions(previous)
    b, _ = _line_positions(current)
    matcher = difflib.SequenceMatcher(None, a, b, autojunk=False)
    mapping = {}
    for tag, old_start, old_end, new_start, new_end in matcher.get_opcodes():
        if tag == "equal":
            mapping.update({i: new_start + i - old_start
                            for i in range(old_start, old_end)})
    return mapping


def preserve_identity(previous_source, current_source, previous_regions):
    """Map prior IDs into rewritten HLSL; never guess ambiguous matches.

    Exact matching uses unchanged diff lines and relative token position on
    those lines; fallback uses unique identical headers within the same scope.
    Inserted loops receive fresh identifiers from discover().
    """
    old_regions = discover(previous_source)
    current_regions = discover(current_source)
    old_lines, old_starts = _line_positions(previous_source)
    new_lines, new_starts = _line_positions(current_source)
    line_map = _mapped_lines(previous_source, current_source)
    assigned = set()
    resolved = []
    for prior in previous_regions:
        old = next((r for r in old_regions if r["start"] == prior.get("start")
                    and r["kind"] == prior.get("kind")), None)
        if old is None or old.get("ambiguous"):
            resolved.append(dict(prior, status="lost"))
            continue
        matches = [r for r in current_regions
                   if r["kind"] == old["kind"] and r.get("syntax") == old.get("syntax")
                   and r["function"] == old["function"] and r["start"] not in assigned]
        old_line = old["line"] - 1
        target_line = line_map.get(old_line)
        exact = [r for r in matches if r["line"] - 1 == target_line
                 and r.get("header") == old.get("header")] if target_line is not None else []
        if len(exact) != 1:
            # A moved or edited line is only safely identifiable by a unique
            # fingerprint. Rewritten loop bounds require explicit re-selection.
            exact = [r for r in matches if r.get("header") == old.get("header")]
        if len(exact) != 1:
            resolved.append(dict(prior, status="ambiguous" if exact else "lost"))
            continue
        chosen = exact[0]
        assigned.add(chosen["start"])
        resolved.append(dict(chosen, id=prior["id"], status="matched",
                             previous_line=old["line"]))
    known = {r["start"] for r in resolved if r.get("status") == "matched"}
    for region in current_regions:
        if region["start"] not in known:
            resolved.append(dict(region, status="new"))
    return sorted(resolved, key=lambda r: (r.get("start", -1), r["id"]))


def apply_identified_regions(source, choices, identified):
    """Apply independent hints against tracked identities, not source indices."""
    if not isinstance(choices, dict):
        raise ValueError("region hints must be an object")
    insertions = []
    changes = []
    mask = _code_mask(source)
    for identifier, mode in choices.items():
        matches = [r for r in identified if r["id"] == identifier]
        if len(matches) != 1 or matches[0].get("status") in ("ambiguous", "lost") or matches[0].get("ambiguous"):
            raise ValueError(f"region identity cannot be resolved safely: {identifier}")
        region = matches[0]
        syntax = region.get("syntax")
        if syntax == "for" and (mode == "loop" or re.fullmatch(r"unroll(?:\\(([1-9]\\d{0,3})\\))?", mode or "")):
            if mode.startswith("unroll(") and int(mode[7:-1]) > 1024:
                raise ValueError("unroll factor must not exceed 1024")
        elif syntax == "if" and mode in ("branch", "flatten"):
            pass
        else:
            raise ValueError(f"hint not supported for {syntax}: {mode}")
        if re.search(r"\\[\\s*\\w+(?:\\s*\\([^\\]]*\\))?\\s*\\]\\s*$", mask[:region["start"]]):
            raise ValueError(f"region already annotated: {identifier}")
        insertions.append((region["start"], f"[{mode}] "))
        changes.append({"id": identifier, "mode": mode, "line": region["line"]})
    for position, inserted in sorted(insertions, reverse=True):
        source = source[:position] + inserted + source[position:]
    return source, changes


def apply_regions(source, choices):
    """Apply several independent statement hints by ID in a single pass.

    choices: {region_id: "loop" | "unroll" | "unroll(N)" | "branch" | "flatten"}.
    IDs are resolved against *this* candidate's source, not stale indices.
    """
    if not isinstance(choices, dict):
        raise ValueError("region hints must be an object")
    regions = discover(source)
    changes = []
    inserts = []
    for ident, mode in choices.items():
        matches = [r for r in regions if r["id"] == ident]
        if len(matches) != 1 or matches[0]["ambiguous"]:
            raise ValueError(f"region ID missing or ambiguous after rewrite: {ident}")
        region = matches[0]
        if region["syntax"] == "for" and re.fullmatch(r"unroll(?:\(([1-9]\d{0,3})\))?|loop", mode or ""):
            if mode.startswith("unroll(") and int(mode[7:-1]) > 1024:
                raise ValueError("unroll count exceeds 1024")
            pass
        elif region["syntax"] == "if" and mode in ("branch", "flatten"):
            pass
        else:
            raise ValueError(f"unsupported {region['syntax']} hint: {mode}")
        # Don't stack attributes, including attributes authored by the model.
        prefix = _code_mask(source)[:region["start"]]
        if re.search(r"\[\s*\w+(?:\s*\([^\]]*\))?\s*\]\s*$", prefix):
            raise ValueError(f"region already has an attribute: {ident}")
        inserts.append((region["start"], f"[{mode}] "))
        changes.append({"id": ident, "mode": mode, "line": region["line"]})
    for pos, value in sorted(inserts, reverse=True):
        source = source[:pos] + value + source[pos:]
    return source, changes


def load_knobs(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("regions"), dict):
        raise ValueError("knob file must contain a regions object")
    return data["regions"]
