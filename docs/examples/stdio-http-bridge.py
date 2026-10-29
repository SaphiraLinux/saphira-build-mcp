#!/usr/bin/env python3
"""Demo-grade stdio-to-HTTP bridge for saphira-build-mcp (stdlib only).

curl cannot speak to a stdio MCP server directly: there is no socket,
no port, no URL -- the transport is the child process's stdin/stdout.
This bridge exists so that curl examples are REAL: curl talks HTTP to
this bridge, and the bridge translates to JSON-RPC/NDJSON on the
server's stdio pipes using the exact framing in docs/jsonrpc-examples.md.

    SBM_NOTES_DB=/tmp/demo-notes.db python3 stdio-http-bridge.py 8473 &
    curl -s localhost:8473/list
    curl -s localhost:8473/call -d '{"tool":"checkpoint_list","arguments":{"limit":2}}'

Endpoints:
  GET  /health            {"ok": true, "server": "<exe>"}
  GET  /list              {"tools": ["checkpoint_save", ...]}
  POST /call              {"tool": "<name>", "arguments": {...}}
                          -> {"ok": true, "text": "..."} or
                             {"ok": false, "error": {...}} / {"ok": false, "is_error": true, ...}

Each request spawns one short-lived server process (initialize,
initialized, one call, EOF). That is deliberate: it keeps the bridge
stateless and matches how agent platforms scope MCP servers per task.

WARNING: localhost demo tool only. No authentication, no TLS, and POST
/call executes journal-writing tools (checkpoint_save, package_notes_add,
package_policy_record) against SBM_NOTES_DB. Never expose beyond loopback,
never point SBM_NOTES_DB at a live journal for curl experiments.
"""
import json
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

EXE = os.environ.get("SBM_EXE", "./saphira-build-mcp")
MAX_BODY = 65536


def mcp_roundtrip(payloads):
    """Send NDJSON requests, return parsed response lines."""
    inp = "".join(json.dumps(p) + "\n" for p in payloads)
    p = subprocess.run([EXE, "--mcp"], input=inp, capture_output=True,
                       text=True, timeout=120)
    out = []
    for line in p.stdout.splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def initialize():
    return [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18",
                    "clientInfo": {"name": "stdio-http-bridge", "version": "1.0"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
    ]


class Handler(BaseHTTPRequestHandler):
    server_version = "sbm-bridge/1.0"

    def _send(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self._send({"ok": True, "server": EXE})
        if self.path == "/list":
            resps = mcp_roundtrip(initialize() + [
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}])
            tools = resps[-1].get("result", {}).get("tools", [])
            return self._send({"tools": [t["name"] for t in tools]})
        return self._send({"ok": False, "error": "unknown path"}, 404)

    def do_POST(self):
        if self.path != "/call":
            return self._send({"ok": False, "error": "unknown path"}, 404)
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length <= 0 or length > MAX_BODY:
            return self._send({"ok": False, "error": "bad body"}, 400)
        try:
            req = json.loads(self.rfile.read(length))
            name = req["tool"]
            args = req.get("arguments", {})
        except (ValueError, KeyError):
            return self._send({"ok": False, "error": "need {tool, arguments}"}, 400)
        resps = mcp_roundtrip(initialize() + [
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
             "params": {"name": name, "arguments": args}}])
        last = resps[-1] if resps else {}
        if "error" in last:
            return self._send({"ok": False, "error": last["error"]})
        result = last.get("result", {})
        if result.get("isError"):
            return self._send({"ok": False, "is_error": True,
                               "text": result["content"][0]["text"]})
        return self._send({"ok": True, "text": result["content"][0]["text"]})

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8473
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
