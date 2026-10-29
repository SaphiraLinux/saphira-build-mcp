# MCP protocol: JSON-RPC 2.0 over stdio (v0.10.8)

Source of truth: `src/mcp.c` (`run_stdio`, `handle_mcp`), `src/mcp.h`.
Every claim below was executed against the v0.10.8 binary; see
`docs/jsonrpc-examples.md` for the runnable forms.

## Transport

- **Primary framing: NDJSON.** One JSON-RPC message per line on stdin;
  one JSON response line per request on stdout, each terminated by `\n`.
- **Fallback framing: LSP-style `Content-Length`.** A message may
  instead arrive as `Content-Length: N\r\n\r\n` (or `\n\n`) followed by
  exactly N bytes. The server sniffs the framing per message: input
  starting with `{` is NDJSON, input starting with `Content-Length:`
  is LSP. Anything else is still answered, never silently dropped
  (an invalid line becomes a JSON-RPC error).
- **Responses match request framing.** NDJSON requests get NDJSON
  responses; `Content-Length` requests get `Content-Length` responses.
- **CRLF tolerated on input, never emitted.** A single trailing `\r`
  is stripped as framing. Server output is LF-only.
- **stdout is protocol, stderr is diagnostics.** Log/diagnostic text
  goes to stderr only. Clients must NOT parse stdout as anything but
  newline-delimited JSON. (The reference clients redirect stderr to
  null; keep it during development.)
- **Binary stdio on Windows.** The server forces `_O_BINARY` on stdin
  and stdout so the CRT cannot translate `\n` to `\r\n` (see
  `docs/windows.md`). stderr stays text mode.
- **EOF ends the session.** When stdin closes, the server exits 0.
  There is no shutdown method; closing the pipe is the shutdown.

## Limits (from `src/mcp.h`)

| Limit | Value | Behaviour on breach |
|---|---|---|
| Request body | 64 KiB (`SBM_MAX_REQUEST_BYTES`) | `-32600` "request too large"; oversize framing bytes are fatal (error emitted, process exits 1) |
| LSP header block | 16 KiB (`SBM_MAX_HEADER_BYTES`) | fatal, same as above |
| Result text | 256 KiB (`SBM_MAX_RESULT_BYTES`) | truncated loudly: cut at 256 KiB plus a trailing `\n[truncated at 256KiB]` marker inside the text |

## Methods

IDs are echoed **raw**: a numeric id stays numeric, a string id stays
a string. Every response carries the request's id, except where noted.

### `initialize`

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"my-client","version":"1.0"}}}
```

Response (`result`):

```json
{"protocolVersion":"2025-06-18","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"saphira-build-mcp","version":"0.10.8"},"instructions":"Session checkpoints are a persistent register of unfinished work ..."}
```

- `protocolVersion` **echoes the client's** when supplied, else the
  default `"2025-03-26"`. The server negotiates nothing; it mirrors.
- `capabilities.tools.listChanged` is always `false`: the tool set is
  fixed at process start (capability env + `tools.conf`), never dynamic.
- `serverInfo` names the product and the `SBM_VERSION` string.
- `instructions` is the live checkpoint-register briefing: static
  guidance plus the current active set, so a new agent learns the
  register without guessing ids. It is read-only and fail-open (an
  unavailable journal yields the static half).
- `clientInfo.name/version`, when supplied, is kept verbatim as the
  **recorder identity** on journal writes (`recorder=name/version`).
  It is client-reported metadata, never trusted for decisions — every
  response footer says so (see `docs/recovery.md`).

### `notifications/initialized`

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

No `id`, no response. Any other id-less message is likewise
notifications-shaped and answered with silence.

### `ping`

```json
{"jsonrpc":"2.0","id":2,"method":"ping"}
```

```json
{"jsonrpc":"2.0","id":2,"result":{}}
```

### `tools/list`

```json
{"jsonrpc":"2.0","id":3,"method":"tools/list"}
```

`result` is `{"tools":[...]}` in registry order: built-ins first
(minus capability-gated and `expose`-off entries), then configured
externals in conf order. Each entry carries `name`, `description`
and `inputSchema` (JSON Schema subset: `type: object`, `properties`,
`required`). The exact set depends on `SBM_SAPHIRA`/`SBM_BUILD` and
`tools.conf` — see "Capabilities" below. `--list-tools` on the
command line prints this same document without starting a session.

### `tools/call`

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{"limit":2}}}
```

Success envelope (always this shape, even for errors of execution):

```json
{"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"..."}],"isError":false}}
```

- A missing, non-string or empty `name` is a protocol error:
  `-32602` "invalid tool call arguments".
- An unknown, capability-gated or `expose`-off tool name is the same
  `-32602` ("unknown tool or invalid arguments") — gated tools are
  **unknown, never failed**. There is no "permission denied" code.
- A tool that runs but reports failure returns `isError: true` with
  the explanation as text (HTTP-200-with-error-body semantics).

### Anything else

`resources/list`, `prompts/list`, completions, roots, sampling —
all answer `-32601` "method not found". This is a **tools-only**
server: no resources, no prompts, no subscriptions.

```json
{"jsonrpc":"2.0","id":10,"method":"resources/list"}
```

```json
{"jsonrpc":"2.0","id":10,"error":{"code":-32601,"message":"method not found"}}
```

## Error catalogue (complete)

| Code | Meaning | Seen when |
|---|---|---|
| `-32700` | parse error | a line is not JSON (`{"jsonrpc":"2.0","id":null,...}`) |
| `-32600` | invalid request | no `method`, non-string `method`, or body over 64 KiB |
| `-32601` | method not found | any method outside initialize / ping / tools/list / tools/call / notifications |
| `-32602` | invalid params | unknown/gated tool name, missing `name`, bad arguments |
| `-32603` | internal error | list assembly or result assembly failed |

## Capabilities: which tools exist

Two environment switches, evaluated once at startup. Anything else
(including empty) **fails the process before serving** — there is no
running degraded:

- `SBM_SAPHIRA=on` (default when unset) / `off`. Off hides the 11
  builder-state tools (`package_search`, `package_info`,
  `package_dependencies`, `package_why_installed`, `package_installed`,
  `package_build_status`, `package_build_history`, `package_build_log`,
  `package_artifacts`, `build_queue`, `package_identity`).
- `SBM_BUILD=on` (default) / `off`. Off hides `build_request` and
  `package_bump` only.

Always on, unswitchable: the journal ten (4 checkpoints, 2 notes,
`total_recall`, 3 policy tools) plus any configured externals.
Proven matrices (v0.10.8): default **23**, build-off **21**,
saphira-off **12**, both off (locked-down) **10**.

## Process lifecycle

```
spawn --mcp -> initialize -> notifications/initialized ->
tools/list, tools/call ... -> close stdin -> exit 0
```

Exit status: 0 on stdin EOF; 1 on internal errors; 2 on usage errors
(bad flags, invalid `SBM_SAPHIRA`/`SBM_BUILD` values).
