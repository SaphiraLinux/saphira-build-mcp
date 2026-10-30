# Remote journals over SSH (v0.10.8)

**One journal. Many agents. Many operating systems. Local or remote
over SSH. Your history stays where you choose.**

MCP already speaks stdin/stdout, and SSH already transports
byte streams securely. No HTTP server, no extra daemon, no sync
protocol: the agent runs locally, SSH launches the MCP executable
remotely, and the SQLite journal never leaves the remote host.

```
OpenCode / Codex (local)
        |
        | MCP JSON-RPC over stdio
        |
       SSH
        |
        v
Remote Linux host
  saphira-build-mcp --mcp
        |
        v
  SQLite journal
  (stays on remote host)
```

Every claim below was executed Egg → Homer over OpenSSH; transcripts
are verbatim (trimmed where marked).

## The command

```sh
ssh -T -o BatchMode=yes user@server \
  'SBM_NOTES_DB=/home/user/.local/share/saphira-build-mcp/notes.db SBM_AUTHOR=egg SBM_SAPHIRA=off exec /usr/bin/saphira-build-mcp --mcp'
```

Each piece is load-bearing:

- `-T`: no pseudo-terminal. A pty would mangle the byte stream
  (echo, CR translation); machine-to-machine MCP must not have one.
- `-o BatchMode=yes`: fail closed instead of prompting. A password
  or host-key prompt mid-handshake would corrupt the JSON-RPC stream
  and stall startup opaquely. Use SSH keys; pre-accept the host key
  out of band.
- `VAR=value ... exec ...`: provenance is set **remotely**.
  `SBM_AUTHOR`, `SBM_NOTES_DB` (and `SBM_SAPHIRA`/`SBM_BUILD` where
  wanted) belong in the remote command, not merely in the local
  client environment — the journal records the remote side's values.
- `exec`: replace the remote shell with the server, so no extra
  process sits in the pipe and shutdown propagates on EOF.

## OpenCode configuration

`type: local` is correct — it names where OpenCode launches the
command (locally); the MCP server itself runs remotely:

```json
"saphira-build-remote": {
  "type": "local",
  "command": [
    "ssh", "-T", "-o", "BatchMode=yes", "smalley@homer",
    "SBM_NOTES_DB=/home/smalley/.local/share/saphira-build-mcp/notes.db SBM_AUTHOR=egg SBM_SAPHIRA=off SBM_TOOLS_CONF=/nonexistent-tools.conf exec /usr/bin/saphira-build-mcp --mcp"
  ],
  "enabled": true
}
```

Verified live: `opencode mcp list` showed
`saphira-build-ssh-test ✓ connected` (that badge already means the
initialize + tools/list handshake completed through SSH), against a
scratch journal, with the config restored byte-identical afterwards.

## Configuration 1: local agent, remote journal, portable mode

One NDJSON line through the pipe; the locked-down surface answers:

```sh
ssh -T -o BatchMode=yes smalley@homer \
  'SBM_SAPHIRA=off SBM_BUILD=off SBM_TOOLS_CONF=/nonexistent-tools.conf exec /usr/bin/saphira-build-mcp --mcp' \
  <<< '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
  | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']['tools']))"
```

```
10
```

Ten tools, and the response parsed as JSON at all — which proves the
second requirement: **the remote shell must print nothing but
protocol.** No MOTD, no `.bashrc` echo, no fortune. Anything on
stdout that is not a JSON-RPC line corrupts framing (stderr is safe
for diagnostics). If `tools/list` does not parse, check the remote
shell startup scripts first.

## Configuration 2: save here, resume there (restart proof)

Checkpoint saved over one SSH connection, resumed over a new one —
the pipe closed in between, so this is a genuine restart:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"ssh-doc-proof","body":"saved over ssh from egg"}}}
```

```json
{"content":[{"type":"text","text":"recorded checkpoint 1 (label 'ssh-doc-proof')\n\n[src: ...]"}],"isError":false}
```

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"label":"ssh-doc-proof"}}}
```

```json
{"content":[{"type":"text","text":"checkpoint [1] (label 'ssh-doc-proof') 2026-10-30T00:0[...] by ssh-doc-egg recorder=docgen/0.10.8\n  where: homer as smalley in /home/smalley (server)\n  scope: (session-global)\n  status: active\n  body:\n  saved over ssh from egg\n\n[src: ...]"}],"isError":false}
```

Note the `where:` line: provenance is **server-observed on the
remote host** (`homer as smalley in /home/smalley (server)`),
independent of where the agent runs. That is what makes cross-host
recovery unambiguous.

## Configuration 3: two agents, one journal, at the same time

Two SSH-launched servers, two authors, one database, writes fired
concurrently. One won, one lost cleanly:

```json
{"content":[{"type":"text","text":"journal write failed\n\n[src: ...]"}],"isError":true}
```

```json
{"content":[{"type":"text","text":"recorded note 1 for ssh-doc [finding] by agent-two\n\n[src: ...]"}],"isError":false}
```

The loser got `journal write failed` with `isError: true` — a loud,
retryable SQLite-busy outcome, never a silent loss. The immediate
retry succeeded (`recorded note 2 ... by agent-one`), and the journal
then showed both rows with correct per-agent authorship. The rules:

- Each server process serves one call at a time; races happen only
  **between** processes (two agents, agent + CLI). The server holds
  no busy-timeout — the error surfaces at once and the caller
  retries. Agent platforms already serialize per server, so this is
  rare and always recoverable.
- Keep the database on a **local filesystem** of the host that runs
  the server. SQLite locking over NFS is unreliable; the supported
  way to share across hosts is SSH transport to a local database,
  not a shared file. (This is also why there is no sync protocol:
  there is one file, one writer discipline, many pipes.)

## Security posture

The server has no authentication of its own — SSH **is** the auth,
the encryption, and the access control. `BatchMode=yes` matters
beyond convenience: without it, a missing key degrades to an
interactive prompt that wedges the MCP handshake. Authorize keys
per host, keep the journal's directory mode `0700`, and remember a
remote `SBM_AUTHOR` is attribution, not identity proof (same as
locally — see `docs/recovery.md`).

## Repeating this verification

1. Scratch journal on the remote host; remove it afterwards.
2. `tools/list` with both capabilities off → `10`, parsed as JSON
   (proves transport, flags, banner silence).
3. `checkpoint_save` on connection one, `checkpoint_resume` on
   connection two (proves restart recovery + remote provenance).
4. Two concurrent `package_notes_add` with distinct `SBM_AUTHOR`
   (proves shared-journal semantics; expect at most a retryable
   busy error, never loss).
5. `opencode mcp list` with a scratch-DB entry (proves client
   integration); restore the config afterwards.
