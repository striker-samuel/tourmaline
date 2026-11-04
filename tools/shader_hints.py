#!/usr/bin/env python3
"""Conservative source-level HLSL hints for controlled compilation experiments.

Only explicit for/if statements are targeted. The scanner masks comments,
strings and preprocessor lines so false positives are not rewritten. The
DXC compiler ultimately decides whether a hint affects generated code.
"""
import re

KEYWORDS = {"loop": "for", "branch": "if"}


def _code_mask(source):
    chars = list(source)
    state = "code"
    i = 0
    while i < len(chars):
        c = source[i]
        following = source[i:i + 2]
        if state == "code":
            if following == "//":
                chars[i:i + 2] = [" ", " "]
                i += 2
                state = "line"
                continue
            if following == "/*":
                chars[i:i + 2] = [" ", " "]
                i += 2
                state = "block"
                continue
            if c in ("'", '"'):
                chars[i] = " "
                state = c
            elif c == "#" and (i == 0 or source[source.rfind("\n", 0, i) + 1:i].strip() == ""):
                chars[i] = " "
                state = "preprocessor"
        elif state in ("line", "preprocessor"):
            if c == "\n":
                state = "code"
            else:
                chars[i] = " "
        elif state == "block":
            if following == "*/":
                chars[i:i + 2] = [" ", " "]
                i += 2
                state = "code"
                continue
            if c != "\n":
                chars[i] = " "
        else:
            if c == "\\" and i + 1 < len(chars):
                chars[i:i + 2] = [" ", " "]
                i += 2
                continue
            if c == state:
                state = "code"
            chars[i] = " "
        i += 1
    return "".join(chars)


def inject_attribute(source, keyword, attribute, index):
    """Insert one hint on the indexed control statement (0-based).

    Reject already-attributed statements to avoid stacking conflicting hints.
    """
    if keyword not in ("for", "if"):
        raise ValueError("only for/if statement hints are supported")
    mask = _code_mask(source)
    matches = list(re.finditer(r"\b" + keyword + r"\s*\(", mask))
    if index < 0 or index >= len(matches):
        raise ValueError(f"{keyword} index {index} not found ({len(matches)} statements)")
    position = matches[index].start()
    prefix = mask[:position]
    # Check for a directly preceding HLSL attribute; replacing unknown
    # attributes without understanding their semantics would be unsafe.
    if re.search(r"\[\s*\w+(?:\s*\([^\]]*\))?\s*\]\s*$", prefix):
        raise ValueError("target statement already has an attribute")
    return source[:position] + f"[{attribute}] " + source[position:]


def apply_hints(source, loop_mode="auto", loop_index=0,
                branch_mode="auto", branch_index=0):
    changes = []
    if loop_mode != "auto":
        if loop_mode not in ("loop", "unroll"):
            raise ValueError("unsupported loop mode")
        source = inject_attribute(source, "for", loop_mode, loop_index)
        changes.append({"kind": "loop", "mode": loop_mode, "index": loop_index})
    if branch_mode != "auto":
        if branch_mode not in ("branch", "flatten"):
            raise ValueError("unsupported branch mode")
        source = inject_attribute(source, "if", branch_mode, branch_index)
        changes.append({"kind": "branch", "mode": branch_mode, "index": branch_index})
    return source, changes
