#!/usr/bin/env python3
"""Minimal standalone MCP stdio client for saphira-build-mcp (stdlib only).

No agent platform required: this script speaks JSON-RPC 2.0 over
newline-delimited JSON (NDJSON) directly to the server's stdin/stdout.

    python3 mcp-stdio-client.py --exe ./saphira-build-mcp list
    python3 mcp-stdio-client.py --exe ./saphira-build-mcp call checkpoint_list '{"limit": 2}'

Works on Linux, macOS and Windows (same framing the PowerShell client
in mcp-stdio-client.ps1 implements). Environment (SBM_NOTES_DB,
SBM_SAPHIRA, SBM_BUILD, SBM_TOOLS_CONF) is inherited by the server
process, exactly as an agent platform would set it.
"""
import argparse
import json
import subprocess
import sys

PROTOCOL_VERSION = "2025-06-18"


class McpStdio:
    def __init__(self, exe):
        self.proc = subprocess.Popen(
            [exe, "--mcp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,  # line-buffered: one JSON-RPC message per line
        )
        self.next_id = 1

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout (EOF)")
        return json.loads(line)

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()
        # notifications get no response

    def close(self):
        try:
            self.proc.stdin.close()
        except BrokenPipeError:
            pass
        self.proc.wait(timeout=10)


def main():
    ap = argparse.ArgumentParser(description="minimal MCP stdio client")
    ap.add_argument("--exe", required=True, help="path to saphira-build-mcp")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init", help="initialize handshake only")
    sub.add_parser("list", help="initialize, then tools/list")
    c = sub.add_parser("call", help="initialize, then tools/call")
    c.add_argument("name")
    c.add_argument("arguments", nargs="?", default="{}")
    args = ap.parse_args()

    mcp = McpStdio(args.exe)
    try:
        init = mcp.request("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "clientInfo": {"name": "mcp-stdio-client", "version": "1.0"},
        })
        mcp.notify("notifications/initialized")
        print(json.dumps({"initialize": init["result"]}, indent=1))
        if args.cmd in ("list", "call"):
            lst = mcp.request("tools/list")
            if args.cmd == "list":
                print(json.dumps([t["name"] for t in lst["result"]["tools"]], indent=1))
            else:
                res = mcp.request("tools/call", {
                    "name": args.name,
                    "arguments": json.loads(args.arguments),
                })
                if "error" in res:
                    print(json.dumps(res["error"], indent=1))
                    return 3
                print(json.dumps(res["result"], indent=1))
        return 0
    finally:
        mcp.close()


if __name__ == "__main__":
    sys.exit(main())
