#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Thin CLI over the Nsight Graphics MCP server (ngfx-mcp.exe).

The MCP server lets an agent (or a script) inspect captures and analyze GPU
traces without driving the Nsight GUI. This script locates ngfx-mcp.exe in a
local Nsight Graphics install, can launch the server (Workflow HTTP transport),
and issues minimal Model Context Protocol (JSON-RPC) requests against it.

Usage:
  python tools/nsight_mcp.py locate                # print the ngfx-mcp.exe path
  python tools/nsight_mcp.py serve [--profile workflow|native] [--port N]
  python tools/nsight_mcp.py list-tools [--port N]
  python tools/nsight_mcp.py inspect <capture.ngfx-capture> [--port N] [--question Q]

Defaults to the latest installed Nsight Graphics on the machine; override with
--mcp PATH (the ngfx-mcp.exe) or the TOURMALINE_NSIGHT_MCP env var.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

DEFAULT_ENDPOINT = "http://127.0.0.1:7661/mcp"


def _find_mcp() -> str:
    """Locate ngfx-mcp.exe in the newest installed Nsight Graphics package."""
    override = os.environ.get("TOURMALINE_NSIGHT_MCP")
    if override:
        return override
    roots = [Path(r"C:\Program Files\NVIDIA Corporation")]
    candidates = []
    for root in roots:
        if not root.exists():
            continue
        for d in root.iterdir():
            if not d.is_dir() or "Nsight Graphics" not in d.name:
                continue
            exe = d / "host" / "windows-desktop-nomad-x64" / "ngfx-mcp.exe"
            if exe.exists():
                candidates.append((d.name, exe))
    if not candidates:
        sys.exit("ngfx-mcp.exe not found; install Nsight Graphics 2026.4+ or set TOURMALINE_NSIGHT_MCP")
    # Prefer the highest version lexicographically.
    candidates.sort(key=lambda c: c[0])
    return str(candidates[-1][1])


_session_id: str | None = None


def _post(endpoint: str, payload: dict) -> tuple[dict, dict]:
    """POST a JSON-RPC message; returns (json_body, headers)."""
    global _session_id
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "MCP-Protocol-Version": "2025-06-18",
    }
    if _session_id:
        headers["Mcp-Session-Id"] = _session_id
    req = urllib.request.Request(
        endpoint,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        sid = resp.headers.get("Mcp-Session-Id")
        if sid:
            _session_id = sid
        raw = resp.read().decode("utf-8")
        body = _parse_body(raw)
        return body, dict(resp.headers)


def _parse_body(raw: str) -> dict:
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        for line in raw.splitlines():
            s = line.strip()
            if s.startswith("data:"):
                try:
                    parsed = json.loads(s[len("data:"):].strip())
                except json.JSONDecodeError:
                    continue
                if isinstance(parsed, dict) and "jsonrpc" in parsed:
                    return parsed
        raise RuntimeError(f"unparseable MCP response: {raw[:200]!r}")


def _rpc(endpoint: str, method: str, params: dict | None = None) -> dict:
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    body, _ = _post(endpoint, payload)
    return body


def _notify(endpoint: str, method: str) -> None:
    # A JSON-RPC notification has no "id" and expects no response.
    payload = {"jsonrpc": "2.0", "method": method, "params": {}}
    try:
        _post(endpoint, payload)
    except urllib.error.HTTPError:
        pass  # the initialized handshake is advisory; ignore a strict server


def _initialize(endpoint: str) -> None:
    _rpc(endpoint, "initialize", {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "tourmaline-nsight-mcp", "version": "1.0"},
    })


def cmd_locate(args: argparse.Namespace) -> int:
    print(_find_mcp())
    return 0


def cmd_serve(args: argparse.Namespace) -> int:
    mcp = args.mcp or _find_mcp()
    port = args.port
    cmd = [
        mcp,
        "--transport", "http",
        "--profile", args.profile,
        "--port", str(port),
    ]
    if args.profile == "native":
        # Native uses stdio by default; force HTTP for a server the agent can reach.
        cmd[cmd.index("--transport") + 1] = "http"
    print(f"starting ngfx-mcp ({args.profile}, http://127.0.0.1:{port}{args.endpoint})")
    return subprocess.call(cmd)


def cmd_list_tools(args: argparse.Namespace) -> int:
    endpoint = f"http://127.0.0.1:{args.port}{args.endpoint}"
    _initialize(endpoint)
    result = _rpc(endpoint, "tools/list", {})
    tools = result.get("result", {}).get("tools", [])
    for t in tools:
        print(f"- {t.get('name')}: {t.get('description', '')[:120]}")
    return 0


def cmd_inspect(args: argparse.Namespace) -> int:
    endpoint = f"http://127.0.0.1:{args.port}{args.endpoint}"
    capture = str(Path(args.capture).resolve())
    question = args.question or (
        "List the draw calls in the first frame and identify their bound shaders. "
        "Start with offline inspection."
    )
    # The Workflow profile advertises an offline-inspection tool; discover its
    # exact name rather than hardcoding the catalog.
    _initialize(endpoint)
    tools = _rpc(endpoint, "tools/list", {}).get("result", {}).get("tools", [])
    inspect_tool = next(
        (t for t in tools if "inspect" in t.get("name", "").lower() or "capture" in t.get("name", "").lower()),
        None,
    )
    if inspect_tool is None:
        print("no inspect/capture tool advertised; available tools:")
        for t in tools:
            print(f"  - {t.get('name')}")
        return 1
    name = inspect_tool["name"]
    result = _rpc(endpoint, "tools/call", {
        "name": name,
        "arguments": {"capture_path": capture, "question": question},
    })
    print(json.dumps(result, indent=2))
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--mcp", help="path to ngfx-mcp.exe (auto-detected if omitted)")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("locate", help="print the ngfx-mcp.exe path")
    s.set_defaults(func=cmd_locate)

    s = sub.add_parser("serve", help="launch the MCP server")
    s.add_argument("--profile", choices=["workflow", "native"], default="workflow")
    s.add_argument("--port", type=int, default=7661)
    s.add_argument("--endpoint", default="/mcp")
    s.set_defaults(func=cmd_serve)

    s = sub.add_parser("list-tools", help="list the MCP server's tools")
    s.add_argument("--port", type=int, default=7661)
    s.add_argument("--endpoint", default="/mcp")
    s.set_defaults(func=cmd_list_tools)

    s = sub.add_parser("inspect", help="inspect a capture via the MCP server")
    s.add_argument("capture")
    s.add_argument("--question", default=None)
    s.add_argument("--port", type=int, default=7661)
    s.add_argument("--endpoint", default="/mcp")
    s.set_defaults(func=cmd_inspect)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
