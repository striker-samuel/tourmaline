#!/usr/bin/env python3
"""Mask HLSL comments, strings and preprocessing for region discovery.

Hints are applied only by hashed region ID in shader_regions.py.
"""

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


