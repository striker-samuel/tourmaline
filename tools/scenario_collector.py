#!/usr/bin/env python3
"""Opt-in localhost HTTP collector for scenario matrix records."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hmac
import json
import os
from pathlib import Path
import re
import uuid

LIMIT = 2 * 1024 * 1024
IDENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")


def validate_record(record):
    if not isinstance(record, dict) or record.get("schema_version") != 1:
        raise ValueError("invalid schema")
    for field in ("machine", "setup", "scenario"):
        if not isinstance(record.get(field), str) or not IDENT.fullmatch(record[field]):
            raise ValueError("invalid " + field)
    if record.get("mode") not in ("cull", "snapshot", "bench"):
        raise ValueError("invalid mode")
    if record.get("status") not in ("pass", "mismatch", "error"):
        raise ValueError("invalid status")
    if not isinstance(record.get("result"), dict):
        raise ValueError("invalid result")
    try:
        uuid.UUID(record["record_id"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("invalid record ID") from exc
    return record


def handler_for(directory, token):
    directory = Path(directory).resolve()

    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, message):
            payload = json.dumps({"message": message}).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            self.reply(200, "ok") if self.path == "/health" else self.reply(404, "not found")

        def do_POST(self):
            if self.path != "/records":
                return self.reply(404, "not found")
            if token and not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                return self.reply(401, "unauthorized")
            length = self.headers.get("Content-Length", "")
            if not length.isdecimal() or not 0 < int(length) <= LIMIT:
                return self.reply(413, "invalid size")
            if self.headers.get_content_type() != "application/json":
                return self.reply(415, "expected JSON")
            try:
                record = validate_record(json.loads(self.rfile.read(int(length))))
                folder = directory / record["machine"] / record["setup"] / record["scenario"]
                folder.mkdir(parents=True, exist_ok=True)
                with (folder / (record["record_id"] + ".json")).open("x", encoding="utf-8") as f:
                    json.dump(record, f, indent=2)
                    f.write("\n")
            except (ValueError, UnicodeError) as exc:
                return self.reply(400, str(exc))
            except FileExistsError:
                return self.reply(409, "record exists")
            except OSError:
                return self.reply(500, "storage error")
            self.reply(201, "stored")
    return Handler


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--output", type=Path, default=Path("out/scenario-records"))
    parser.add_argument("--token", default=os.environ.get("TOURMALINE_RECORD_TOKEN", ""))
    args = parser.parse_args(argv)
    if args.host not in ("127.0.0.1", "::1", "localhost") and not args.token:
        parser.error("non-loopback binding requires token")
    if not 0 <= args.port <= 65535:
        parser.error("invalid port")
    args.output.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), handler_for(args.output, args.token))
    print("collector: %s:%d" % (args.host, server.server_port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
