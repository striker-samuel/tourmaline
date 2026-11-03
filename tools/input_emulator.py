#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Controlled keyboard/mouse input replay for Tourmaline GUI smoke testing.

Validates a JSONL event script without touching the desktop by default.
Live replay requires BOTH --execute and an explicitly chosen target window
title (--window), with a foreground-window check before every action.
Windows only for live replay (pyautogui + pygetwindow); validation works
on any Python 3.9+ system. Does not install hooks or record user keystrokes.

  python tools/input_emulator.py validate tools/input_example.jsonl --json
  python tools/input_emulator.py run tools/input_example.jsonl --dry-run --jsonl
  python tools/input_emulator.py run script.jsonl --execute --window 'Tourmaline'
  cat script.jsonl | python tools/input_emulator.py run --stdin --dry-run --jsonl

Input event JSONL (one object per line):
  {"action":"key","key":"tab"}
  {"action":"key_down","key":"w"}
  {"action":"wait","seconds":0.2}
  {"action":"key_up","key":"w"}
  {"action":"click","x":200,"y":150,"button":"left"}
  {"action":"move","x":300,"y":200}
  {"action":"scroll","clicks":-2}
  {"action":"text","text":"demo"}
  {"action":"screenshot","path":"out/input-check.png"}

Coordinates are screen pixels. Waits are bounded; screenshots may only
be written explicitly. No arbitrary shell commands or code evaluation.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

ACTIONS = {"key", "key_down", "key_up", "wait", "click", "move",
           "scroll", "text", "screenshot"}
ALLOWED_KEYS = {
    "tab", "enter", "return", "esc", "escape", "space", "backspace", "delete",
    "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
    "shift", "ctrl", "alt", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8",
    "f9", "f10", "f11", "f12", "insert", "capslock"
} | set("abcdefghijklmnopqrstuvwxyz0123456789")
ALLOWED_BUTTONS = {"left", "middle", "right"}
MAX_EVENTS = 1000
MAX_WAIT = 10.0
MAX_TOTAL_WAIT = 60.0


class InputError(Exception):
    pass


def checked_int(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise InputError(f"{name} must be an integer in [{low}, {high}]")
    return value


def check_event(event, number):
    if not isinstance(event, dict):
        raise InputError(f"line {number}: expected JSON object")
    action = event.get("action")
    if action not in ACTIONS:
        raise InputError(f"line {number}: unknown action {action!r}")
    schema = {
        "key": {"key"}, "key_down": {"key"}, "key_up": {"key"},
        "wait": {"seconds"}, "click": {"x", "y", "button"},
        "move": {"x", "y"}, "scroll": {"clicks"},
        "text": {"text"}, "screenshot": {"path"},
    }
    permitted = schema[action] | {"action"}
    unexpected = set(event) - permitted
    if unexpected:
        raise InputError(f"line {number}: unsupported fields {sorted(unexpected)}")
    if action in ("key", "key_down", "key_up"):
        if event.get("key") not in ALLOWED_KEYS:
            raise InputError(f"line {number}: unsupported key {event.get('key')!r}")
    elif action == "wait":
        value = event.get("seconds")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= MAX_WAIT:
            raise InputError(f"line {number}: wait must be between 0 and {MAX_WAIT} seconds")
    elif action in ("click", "move"):
        checked_int(event.get("x"), "x", 0, 32767)
        checked_int(event.get("y"), "y", 0, 32767)
        if action == "click" and event.get("button", "left") not in ALLOWED_BUTTONS:
            raise InputError(f"line {number}: invalid mouse button")
    elif action == "scroll":
        checked_int(event.get("clicks"), "clicks", -20, 20)
    elif action == "text":
        text = event.get("text")
        if not isinstance(text, str) or len(text) > 512 or any(ord(c) < 32 and c not in "\n\t" for c in text):
            raise InputError(f"line {number}: text must be <=512 printable characters")
    elif action == "screenshot":
        path = event.get("path")
        if not isinstance(path, str) or not path or len(path) > 512 or Path(path).suffix.lower() != ".png":
            raise InputError(f"line {number}: screenshot path must end in .png")
    return {"action": action, **{k: v for k, v in event.items() if k != "action"}}


def parse_script(lines):
    events = []
    duration = 0.0
    pressed = set()
    for n, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if len(line) > 8192:
            raise InputError(f"line {n}: input line too long")
        try:
            raw = json.loads(line)
        except ValueError as exc:
            raise InputError(f"line {n}: invalid JSON: {exc}") from exc
        event = check_event(raw, n)
        if event["action"] == "wait":
            duration += event["seconds"]
            if duration > MAX_TOTAL_WAIT:
                raise InputError("total scripted waits exceed 60 seconds")
        if event["action"] == "key_down":
            if event["key"] in pressed:
                raise InputError(f"line {n}: duplicate key_down for {event['key']}")
            pressed.add(event["key"])
        elif event["action"] == "key_up":
            if event["key"] not in pressed:
                raise InputError(f"line {n}: key_up without key_down for {event['key']}")
            pressed.remove(event["key"])
        events.append(event)
        if len(events) > MAX_EVENTS:
            raise InputError(f"script exceeds {MAX_EVENTS} events")
    if pressed:
        raise InputError(f"script leaves keys pressed: {sorted(pressed)}")
    if not events:
        raise InputError("no events to replay")
    return events


def execute(events, title):
    if os.name != "nt":
        raise InputError("live desktop input is currently supported on Windows only")
    try:
        import pyautogui as gui
        import pygetwindow as gw
    except ImportError as exc:
        raise InputError("install live dependencies: pip install pyautogui pygetwindow") from exc

    gui.FAILSAFE = True  # physical pointer at upper-left corner aborts
    gui.PAUSE = 0.02
    pressed = set()

    def require_focus():
        active = gw.getActiveWindow()
        if active is None or title.casefold() not in (active.title or "").casefold():
            raise InputError(f"active window does not match --window {title!r}")
    try:
        for event in events:
            require_focus()
            action = event["action"]
            if action == "wait":
                time.sleep(event["seconds"])
            elif action == "key":
                gui.press(event["key"])
            elif action == "key_down":
                gui.keyDown(event["key"])
                pressed.add(event["key"])
            elif action == "key_up":
                gui.keyUp(event["key"])
                pressed.remove(event["key"])
            elif action == "click":
                gui.click(x=event["x"], y=event["y"], button=event.get("button", "left"))
            elif action == "move":
                gui.moveTo(event["x"], event["y"], duration=0)
            elif action == "scroll":
                gui.scroll(event["clicks"])
            elif action == "text":
                gui.write(event["text"], interval=0.005)
            elif action == "screenshot":
                target = Path(event["path"])
                target.parent.mkdir(parents=True, exist_ok=True)
                gui.screenshot().save(str(target))
    finally:
        for key in sorted(pressed):
            gui.keyUp(key)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("validate", "run"))
    ap.add_argument("script", nargs="?", help="JSONL script file")
    ap.add_argument("--stdin", action="store_true", help="read event JSONL from stdin")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--jsonl", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="validate and print events; no desktop input")
    ap.add_argument("--execute", action="store_true", help="explicitly enable desktop input")
    ap.add_argument("--window", help="required substring of active foreground window title")
    args = ap.parse_args(argv)
    if args.stdin == bool(args.script):
        ap.error("provide exactly one of a script filename or --stdin")
    if args.execute and (args.dry_run or args.command == "validate"):
        ap.error("--execute is incompatible with --dry-run / validate")
    if args.execute and not args.window:
        ap.error("--execute requires --window")
    try:
        if args.stdin:
            events = parse_script(sys.stdin)
        else:
            with open(args.script, encoding="utf-8") as stream:
                events = parse_script(stream)
        if args.execute:
            execute(events, args.window)
        result = {"schema_version": 1, "valid": True, "executed": args.execute,
                  "event_count": len(events),
                  "wait_seconds": sum(e.get("seconds", 0) for e in events)}
        if args.jsonl:
            for i, event in enumerate(events):
                print(json.dumps({"index": i, **event}, sort_keys=True))
        elif args.json:
            print(json.dumps(result, sort_keys=True))
        else:
            print(f"input emulator: {len(events)} events, "
                  f"{'executed' if args.execute else 'validated (dry run)'}")
        return 0
    except (InputError, OSError) as exc:
        print(f"input_emulator: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
