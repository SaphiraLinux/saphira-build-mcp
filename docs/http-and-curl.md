# curl and the stdio MCP server (v0.10.8)

**curl cannot invoke this server directly.** There is no socket, no
port, no URL, no HTTP handler in the binary — the transport is the
child process's stdin/stdout (see `docs/mcp-protocol.md`). Any document
showing `curl http://.../tools/call` against the bare executable is
inventing an endpoint. This file shows the honest alternative: a tiny
localhost bridge that curl *can* talk to, with every byte verified.

## The bridge: `docs/examples/stdio-http-bridge.py`

Stdlib-only Python. curl speaks HTTP to the bridge; the bridge spawns
the MCP server per request and translates, using the exact framing in
`docs/jsonrpc-examples.md`:

```sh
export SBM_NOTES_DB=/tmp/sbm-bridge-demo-notes.db   # NEVER a live journal
export SBM_TOOLS_CONF=/nonexistent-tools.conf
export SBM_EXE=./saphira-build-mcp
python3 docs/examples/stdio-http-bridge.py 8473 &
```

Endpoints: `GET /health`, `GET /list`, `POST /call` with
`{"tool": "<name>", "arguments": {...}}`. Verified transcript:

```sh
$ curl -s localhost:8473/health
{"ok": true, "server": "./saphira-build-mcp"}

$ curl -s localhost:8473/list | python3 -c "import json,sys; print(len(json.load(sys.stdin)['tools']))"
23

$ curl -s localhost:8473/call -d '{"tool":"checkpoint_list","arguments":{"limit":2}}'
{"ok": true, "text": "(no active checkpoints)\n\n[src: notes.db (local journal; recorder is client-reported, unverified)]"}

$ curl -s localhost:8473/call -d '{"tool":"no_such_tool","arguments":{}}'
{"ok": false, "error": {"code": -32602, "message": "unknown tool or invalid arguments"}}
```

Every per-tool `curl` line in `docs/tools.md` is this pattern with a
different body — same code path, verified here. A journal-writing call
through the bridge returns `{"ok": true, ...}` exactly like a read;
a tool-level failure returns `{"ok": false, "is_error": true, ...}`.

## Safety rules (the bridge has none of its own)

- Bind is `127.0.0.1` only; no auth, no TLS. Localhost demo tool.
- `POST /call` executes journal writes (`checkpoint_save`,
  `package_notes_add`, `package_policy_record`) against `SBM_NOTES_DB`.
  Point it at a scratch journal, never a live one.
- Privileged tools stay privileged: with `SBM_BUILD=off` the bridge
  faithfully returns `-32602` for `build_request`/`package_bump`,
  because the server does.
- Prefer the Python stdio client (`docs/examples/mcp-stdio-client.py`)
  for real work: one process, no HTTP hop, no extra attack surface.
  The bridge exists to make curl examples *true*, not to be deployed.
