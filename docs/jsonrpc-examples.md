# JSON-RPC over stdio: copy-and-paste examples (v0.10.8)

No agent platform needed. Every example below was executed against the
v0.10.8 binary; outputs are verbatim (trimmed where marked).

Setup for all examples (throwaway journal, portable surface):

```sh
export SBM_NOTES_DB=/tmp/sbm-doc-demo-notes.db
export SBM_TOOLS_CONF=/nonexistent-tools.conf   # built-ins only
BIN=./saphira-build-mcp
```

## One-shot calls with printf and a pipe

Each `printf ... | $BIN --mcp` starts a session, answers, and exits on EOF.

List tool names:

```sh
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | $BIN --mcp \
 | python3 -c "import json,sys; print('\n'.join(t['name'] for t in json.load(sys.stdin)['result']['tools']))"
```

Call a read-only tool (queue is empty on a fresh host — fine, that is
the honest answer):

```sh
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"build_queue","arguments":{"limit":1}}}' \
 | $BIN --mcp
```

```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"queue empty\n\n[src: build queue]"}],"isError":false}}
```

Ping, and the CRLF tolerance (a stray `\r` is framing, not content):

```sh
printf '{"jsonrpc":"2.0","id":8,"method":"ping"}\r\n' | $BIN --mcp
```

```json
{"jsonrpc":"2.0","id":8,"result":{}}
```

Garbage in, JSON-RPC out (never silence):

```sh
printf 'hello\n' | $BIN --mcp
printf '{bad\n' | $BIN --mcp
```

```json
{"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error"}}
```

```json
{"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error"}}
```

Unknown tool and unknown method:

```sh
printf '%s\n' \
 '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"no_such_tool","arguments":{}}}' \
 '{"jsonrpc":"2.0","id":10,"method":"resources/list"}' | $BIN --mcp
```

```json
{"jsonrpc":"2.0","id":9,"error":{"code":-32602,"message":"unknown tool or invalid arguments"}}
{"jsonrpc":"2.0","id":10,"error":{"code":-32601,"message":"method not found"}}
```

## LSP `Content-Length` framing

Accepted as a fallback per message; the response mirrors the framing:

```sh
python3 - <<'EOF' | $BIN --mcp
import json
body = json.dumps({"jsonrpc":"2.0","id":7,"method":"initialize",
                   "params":{"protocolVersion":"2025-06-18"}})
import sys
sys.stdout.write("Content-Length: %d\r\n\r\n%s" % (len(body), body))
EOF
```

```
Content-Length: 693\r\n\r\n{"jsonrpc":"2.0","id":7,"result":{"protocolVersion":"2025-06-18",...}}
```

(The 693-byte body is the full initialize result with the live
checkpoint-register instructions; trimmed here.)

## Full session in Python (subprocess)

For anything beyond one-shots, keep one process alive and speak
line-delimited JSON. The complete, maintained version is
`docs/examples/mcp-stdio-client.py` (stdlib only; verified: initialize
+ tools/list returning all 23 tool names). Its shape:

```python
import json, subprocess
p = subprocess.Popen(["./saphira-build-mcp", "--mcp"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
rid = 1
def request(method, params=None):
    global rid
    msg = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        msg["params"] = params
    rid += 1
    p.stdin.write(json.dumps(msg) + "\n")   # <-- the trailing newline IS the framing
    p.stdin.flush()
    return json.loads(p.stdout.readline())  # one response line per request

print(request("initialize", {"protocolVersion": "2025-06-18",
      "clientInfo": {"name": "demo", "version": "1.0"}})["result"]["serverInfo"])
request("notifications/initialized") if False else p.stdin.write(
    json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
print([t["name"] for t in request("tools/list")["result"]["tools"]][:3])
print(request("tools/call", {"name": "checkpoint_list",
      "arguments": {"limit": 1}})["result"]["content"][0]["text"][:60])
p.stdin.close(); p.wait(timeout=10)   # EOF is the shutdown; server exits 0
```

Rules the client must obey (all load-bearing, all in `src/mcp.c`):

1. Flush after every line; read exactly one line per request.
2. Send `notifications/initialized` after `initialize` (no reply comes).
3. Never write to the pipe after closing; never parse stdout as text.
4. `stderr` is diagnostics — inherit it while debugging, null it in production.
5. Request bodies over 64 KiB are refused (`-32600`); results over
   256 KiB arrive truncated with a trailing marker — size your reads.

## Same session in PowerShell

`docs/examples/mcp-stdio-client.ps1` implements the identical flow
natively on Windows (`-Exe .\saphira-build-mcp.windows-builtin.exe
-Cmd list|call`). It is review-validated against the Python client and
the framing code; it is not executed in Linux CI (no PowerShell
runtime there — see `docs/windows.md` for the Wine-tested path).
