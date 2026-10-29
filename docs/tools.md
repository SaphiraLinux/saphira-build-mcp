# Tools reference (v0.10.8, 23 built-ins)

Every section: capability badge, mutation class, parameters (from the
live `inputSchema`), a valid `tools/call` example, and a **real
captured response** (trimmed excerpts marked `[...]`, full transcripts
in the verification lab). `curl` lines go through the localhost bridge
in `docs/http-and-curl.md`; PowerShell lines use
`docs/examples/mcp-stdio-client.ps1` (review-validated; see its header
note about Linux CI).

Badges: **JOURNAL** always listed · **BUILD-STATE** needs
`SBM_SAPHIRA=on` · **BUILD-OP** needs `SBM_BUILD=on`.
Mutation: **READ-ONLY** · **JOURNAL-WRITE** (appends to `SBM_NOTES_DB`,
never touches the system) · **PRIVILEGED** (drives the builder through
sanctioned surfaces; signing/publishing/installing have no tools at all).

## `checkpoint_save` — JOURNAL · JOURNAL-WRITE

Save a pre-restart session checkpoint. Active by default; resume later
by id, by label (newest), or newest overall.

| Param | Required | Notes |
|---|---|---|
| `label` | yes | short handle, e.g. `pre-compact-0108-done` |
| `body` | yes | state, SHAs, outstanding work, next action |
| `title`, `package`, `project`, `workstream` | no | grouping |
| `src_commit`, `recipe_commit` | no | pin the sources |
| `author`, `session_id`, `hostname`, `username`, `cwd`, `agent` | no | provenance overrides (else server-observed) |

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"docgen-probe","body":"documentation capture probe"}}}
```

```json
{"content":[{"type":"text","text":"recorded checkpoint 1 (label 'docgen-probe')\n\n[src: notes.db (local journal; recorder is client-reported, unverified)]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"checkpoint_save","arguments":{"label":"pre-restart","body":"state, SHAs, next"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name checkpoint_save -Arguments '{"label":"pre-restart","body":"state, SHAs, next"}'
```

## `checkpoint_resume` — JOURNAL · READ-ONLY

All params optional (`id`, `label`); omit both for newest overall.
Closed checkpoints stay resumable by id.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"label":"docgen-probe"}}}
```

```json
{"content":[{"type":"text","text":"checkpoint [1] (label 'docgen-probe') 2026-10-29T23:45:58Z by docgen recorder=docgen/0.10.8\n  where: egg.saphira.vm2.uk as smalley in /home/smalley/saphira-build-mcp (server)\n  scope: (session-global)\n  status: active\n  body:\n  documentation capture probe\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"checkpoint_resume","arguments":{"label":"pre-restart"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name checkpoint_resume -Arguments '{"label":"pre-restart"}'
```

## `checkpoint_list` — JOURNAL · READ-ONLY

Active register, newest first, with total count. Optional `status`
(active/closed/all), `project`, `workstream`, `label`, `limit`,
`include_closed`.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{"limit":5}}}
```

```json
{"content":[{"type":"text","text":"active checkpoints: 1 total (showing 1)\n  [1] label 'docgen-probe' by docgen at 2026-10-29T23:45:58Z\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"checkpoint_list","arguments":{"limit":5}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name checkpoint_list -Arguments '{"limit":5}'
```

## `checkpoint_close` — JOURNAL · JOURNAL-WRITE

Retire by `id` (required). History is preserved, never deleted.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_close","arguments":{"id":1}}}
```

```sh
curl -s localhost:8473/call -d '{"tool":"checkpoint_close","arguments":{"id":1}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name checkpoint_close -Arguments '{"id":1}'
```

## `package_notes_add` — JOURNAL · JOURNAL-WRITE

Record a finding/decision/issue/success/failure/lesson.
`package`, `kind`, `body` required; `title` + provenance overrides optional.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"docgen-probe","kind":"finding","title":"probe","body":"documentation capture probe"}}}
```

```json
{"content":[{"type":"text","text":"recorded note 1 for docgen-probe [finding] by docgen\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_notes_add","arguments":{"package":"saphira-build-mcp","kind":"finding","title":"t","body":"b"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_notes_add -Arguments '{"package":"saphira-build-mcp","kind":"finding","title":"t","body":"b"}'
```

## `package_notes_get` — JOURNAL · READ-ONLY

Newest first. `package` required; `kind`, `limit` optional.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_notes_get","arguments":{"package":"docgen-probe","limit":5}}}
```

```json
{"content":[{"type":"text","text":"notes for docgen-probe:\n[1] finding 2026-10-29T23:45:58Z by docgen recorder=docgen/0.10.8\n  title: probe\n  documentation capture probe\n(showing 1)\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_notes_get","arguments":{"package":"saphira-build-mcp","limit":2}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_notes_get -Arguments '{"package":"saphira-build-mcp","limit":2}'
```

## `package_policy_record` — JOURNAL · JOURNAL-WRITE

Attach verified evidence to one of the five checks
(`unix-identity`, `payload-layout`, `revision-discipline`,
`dependency-closure`, `signing-acceptance`). `package`, `check`,
`result` (`PASS`/`FAIL`) required; `evidence` object + provenance
overrides optional. Supersedes the prior record for that check.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_policy_record","arguments":{"package":"docgen-probe","check":"revision-discipline","result":"PASS","evidence":{}}}}
```

```json
{"content":[{"type":"text","text":"recorded revision-discipline=PASS for docgen-probe (record 1, first record)\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_policy_record","arguments":{"package":"p","check":"revision-discipline","result":"PASS","evidence":{}}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_policy_record -Arguments '{"package":"p","check":"revision-discipline","result":"PASS","evidence":{}}'
```

## `package_policy_status` — JOURNAL · READ-ONLY

Checks with evidence and freshness. `package` required.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_policy_status","arguments":{"package":"docgen-probe"}}}
```

```json
{"content":[{"type":"text","text":"policy status for docgen-probe:\nunix-identity: UNVERIFIED (no record)\npayload-layout: UNVERIFIED (no record)\nrevision-discipline: PASS CURRENT (record 1, 2026-10-29T23:45:59Z by docgen)\n  evidence: {}\ndependency-closure: UNVERIFIED (no record)\nsigning-acceptance: UNVERIFIED (no record)\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_policy_status","arguments":{"package":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_policy_status -Arguments '{"package":"saphira-build-mcp"}'
```

## `package_readiness` — JOURNAL · READ-ONLY

Verified / stale / unverified roll-up plus open issues and recent
notes. `package` required.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_readiness","arguments":{"package":"docgen-probe"}}}
```

```json
{"content":[{"type":"text","text":"readiness for docgen-probe:\nverified (current):\n  revision-discipline: PASS CURRENT (record 1, 2026-10-29T23:45:59Z by docgen)\n  evidence: {}\nstale (needs revalidation):\n  (none)\nunverified:\n  unix-identity, payload-layout, dependency-closure, signing-acceptance\nrecent:\n  [1] finding 2026-10-29T23:45:58Z by docgen [...]"} }],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_readiness","arguments":{"package":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_readiness -Arguments '{"package":"saphira-build-mcp"}'
```

## `total_recall` — JOURNAL · READ-ONLY

Bounded historical retrieval. All params optional: `mode`
(`query`/`timeline`/`export`), `query`, `session_id`, `package`,
`label`, `project`, `author`, `hostname`, `username`, `cwd`,
`detail` (`summary`/`full`), `budget` (`compact` 4 KiB / `standard`
16 KiB / `extended` 64 KiB / `deep` 256 KiB), `limit` (clamped to
100), `cursor` (`o<offset>` plus creation/table/id cursors).

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"total_recall","arguments":{"mode":"query","query":"probe","budget":"compact","limit":2,"detail":"summary"}}}
```

```json
{"content":[{"type":"text","text":"total_recall query, budget compact (4KiB), 2 rows:\n  scope: query probe\n  searched: checkpoints, notes, policy records, tool runs, session events\n  not searched: tool outputs and conversation turns (never captured)\nenvironments (1):\n  [1] egg.saphira.vm2.uk as smalley in /home/smalley/saphira-build-mcp (server)\n- [notes #1] probe | session - 2026-10-29T [...]"}],"isError":false}
```

Timeline and export share the envelope (`timeline, ... 2 rows`,
`export, ... 1 row`, same scope/footer). See `docs/recovery.md` for
budgets, cursors and the never-captured rule.

```sh
curl -s localhost:8473/call -d '{"tool":"total_recall","arguments":{"mode":"timeline","budget":"compact","limit":2}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name total_recall -Arguments '{"mode":"timeline","budget":"compact","limit":2}'
```

## `package_search` — BUILD-STATE · READ-ONLY

Name/description substring over recipes. `query` required; `limit`,
`include_disabled` optional. Disabled recipes are skipped unless asked.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_search","arguments":{"query":"saphira-build-mcp","limit":2}}}
```

```json
{"content":[{"type":"text","text":"recipes:\nsaphira-build-mcp 0.10.8-r1 - Read-only Saphira build/package queries for AI agents (local MCP stdio server, tools.conf extensions)\nrepository:\nsaphira-build-mcp saphira-build-mcp-0.10.8-r1\nsaphira-build-mcp-doc saphira-build-mcp-doc-0.10.8-r1\n(capped: refine the query)\n\n[src: recipes, repository.db]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_search","arguments":{"query":"saphira-build-mcp","limit":2}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_search -Arguments '{"query":"saphira-build-mcp","limit":2}'
```

## `package_info` — BUILD-STATE · READ-ONLY

Recipe fields + repository newest. `name` required.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_info","arguments":{"name":"saphira-build-mcp"}}}
```

```json
{"content":[{"type":"text","text":"name: saphira-build-mcp\nversion: 0.10.8\nrevision: 1\ndescription: Read-only Saphira build/package queries for AI agents [...]\nlicense: BUSL-1.1\norigin: saphira-build-mcp\ndisabled: no\nsubpackages: $pkgname-doc\nrepo newest: saphira-build-mcp-0.10.8-r1 (published 2026-10-29T23:24:47Z)\n\n[src: recipes, repository.db]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_info","arguments":{"name":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_info -Arguments '{"name":"saphira-build-mcp"}'
```

## `package_dependencies` — BUILD-STATE · READ-ONLY

Dependency edges. `name` required; `direction` optional
(`deps`/`reverse`/`both`).

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_dependencies","arguments":{"name":"saphira-build-mcp","direction":"deps"}}}
```

```json
{"content":[{"type":"text","text":"dependencies:\ndep: sqlite\ndep: gcc-libs\n\n[src: apk dot]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_dependencies","arguments":{"name":"saphira-build-mcp","direction":"deps"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_dependencies -Arguments '{"name":"saphira-build-mcp","direction":"deps"}'
```

## `package_why_installed` — BUILD-STATE · READ-ONLY

Dependency chain(s) from package to dep. Both required. A pair with
no chain is honest text, not an error (`isError` stays false):

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_why_installed","arguments":{"package":"saphira-build-mcp","dep":"sqlite"}}}
```

```json
{"content":[{"type":"text","text":"chain: saphira-build-mcp -> sqlite\nchain: saphira-build-mcp -> sqlite\n\n[src: apk dot]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_why_installed","arguments":{"package":"saphira-build-mcp","dep":"sqlite"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_why_installed -Arguments '{"package":"saphira-build-mcp","dep":"sqlite"}'
```

## `package_installed` — BUILD-STATE · READ-ONLY

Installed version versus repository newest. `name` required.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_installed","arguments":{"name":"saphira-build-mcp"}}}
```

```json
{"content":[{"type":"text","text":"installed: 0.10.8-r1 [...]\nrepository: saphira-build-mcp-0.10.8-r1\nverdict: differ\n\n[src: installed DB, repository.db]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_installed","arguments":{"name":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_installed -Arguments '{"name":"saphira-build-mcp"}'
```

## `package_build_status` — BUILD-STATE · READ-ONLY

Latest build verdict, timings, next step. `name` required;
`include_disabled` optional.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_build_status","arguments":{"name":"saphira-build-mcp"}}}
```

```json
{"content":[{"type":"text","text":"saphira-build-mcp 0.10.8-r1: AWAITING SIGNING\n  wall 5.0 cpu 2.074 rss 1321.2 arch x86_64 builder egg.saphira.vm2.uk finished 2026-10-29T23:22:49Z\n  next: built and unsigned - ask the operator to run `sudo sign-apk-repo saphira-build-mcp`\nSBU baseline: none active for saphira-build-mcp [...]\n\n[src: build queue]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_build_status","arguments":{"name":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_build_status -Arguments '{"name":"saphira-build-mcp"}'
```

## `package_build_history` — BUILD-STATE · READ-ONLY

Previous attempts with timings and SBU. `name` required; `limit`,
`include_disabled` optional.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_build_history","arguments":{"name":"saphira-build-mcp","limit":1}}}
```

```json
{"content":[{"type":"text","text":"saphira-build-mcp 0.10.8-r1: AWAITING SIGNING\n  wall 5.0 cpu 2.074 rss 1321.2 arch x86_64 builder egg.saphira.vm2.uk finished 2026-10-29T23:22:49Z\n(showing 1)\nSBU baseline: none active for saphira-build-mcp [...]\n\n[src: build queue]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_build_history","arguments":{"name":"saphira-build-mcp","limit":1}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_build_history -Arguments '{"name":"saphira-build-mcp","limit":1}'
```

## `package_build_log` — BUILD-STATE · READ-ONLY

Retained log tail. `name` required; `stage` (`build`/`install`/`both`),
`tail_bytes` optional. Keep tails small (results truncate at 256 KiB).

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_build_log","arguments":{"name":"saphira-build-mcp","stage":"both","tail_bytes":512}}}
```

```json
{"content":[{"type":"text","text":"[/var/lib/saphira-build/logs/saphira-build-mcp/0.10.8-r1-c87dda3a/build.log: 206 bytes]\nbuildpkg: unsigned artifacts ready at /out/stage4/incoming/x86_64/saphira-build-mcp-79f048da4ac7-ready\nbuildpkg: built saphira-build-mcp; 1 authoritative transaction(s) staged\n\n$ buildpkg saphira-build-mcp\n[...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_build_log","arguments":{"name":"saphira-build-mcp","stage":"both","tail_bytes":512}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_build_log -Arguments '{"name":"saphira-build-mcp","stage":"both","tail_bytes":512}'
```

## `package_artifacts` — BUILD-STATE · READ-ONLY

Staged, signed and published artifacts. `name` required.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_artifacts","arguments":{"name":"saphira-build-mcp"}}}
```

```json
{"content":[{"type":"text","text":"staged:\n(superseded) saphira-build-mcp-f67d04cac4c5-ready.superseded-rebuilt\n(superseded) saphira-build-mcp-e47b43c2a043-ready.superseded-rebuilt\n[...]\n\n[src: ...]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_artifacts","arguments":{"name":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_artifacts -Arguments '{"name":"saphira-build-mcp"}'
```

## `build_queue` — BUILD-STATE · READ-ONLY

Queued and active builds. `limit` optional.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"build_queue","arguments":{"limit":1}}}
```

```json
{"content":[{"type":"text","text":"queue empty\n\n[src: build queue]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"build_queue","arguments":{"limit":1}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name build_queue -Arguments '{"limit":1}'
```

## `package_identity` — BUILD-STATE · READ-ONLY

Unix identity: fragment, registry, reservations, live accounts.
`name` required. Absent identities are reported, not errored:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_identity","arguments":{"name":"saphira-build-mcp"}}}
```

```json
{"content":[{"type":"text","text":"fragment:\n(no fragment declared)\nregistry (accounts.tsv):\n(no registry rows)\nreservations (repository.db):\n(none registered)\nlive user:\nsaphira-build-mcp: absent\nlive group:\nsaphira-build-mcp: absent\n\n[src: accounts.d, accounts.tsv, repository.db, live accounts]"}],"isError":false}
```

```sh
curl -s localhost:8473/call -d '{"tool":"package_identity","arguments":{"name":"saphira-build-mcp"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_identity -Arguments '{"name":"saphira-build-mcp"}'
```

## `build_request` — BUILD-OP · PRIVILEGED

Queue a build through the daemon intake (`scan`); the daemon executes
detached. `package`, `reason` required; `author` optional. The reason
is journaled. **Never signs, publishes or installs** — those have no
tools and stay human-only.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"saphira-build-mcp","reason":"0.10.8 retry: <why>"}}}
```

Live response (build 5846, recorded during the 0.10.8 release):

```json
{"content":[{"type":"text","text":"build request for saphira-build-mcp (scan exit 0):\n  reason: 0.10.8 retry: [...]\n  queued row 5846: QUEUED\nnote 136 recorded: build requested\nthe builder daemon executes detached; follow with build_queue / package_build_status [...]"}],"isError":false}
```

Safe to document, not safe to click: do NOT execute this against a
live builder from curl experiments. Verify the shape via the refusal
below instead.

```sh
# read-only proof the gate exists (SBM_BUILD=off):
SBM_BUILD=off curl -s localhost:8473/call -d '{"tool":"build_request","arguments":{"package":"x","reason":"y"}}'
# {"ok": false, "error": {"code": -32602, "message": "unknown tool or invalid arguments"}}
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name build_request -Arguments '{"package":"x","reason":"y"}'
```

## `package_bump` — BUILD-OP · PRIVILEGED

Revision bump with `bumppkg` guards; the reason is journaled. Never
signs, publishes or installs. Same shape and same warning as
`build_request`.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_bump","arguments":{"package":"saphira-build-mcp","reason":"ship updated man page"}}}
```

```sh
SBM_BUILD=off curl -s localhost:8473/call -d '{"tool":"package_bump","arguments":{"package":"x","reason":"y"}}'
```

```powershell
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call -Name package_bump -Arguments '{"package":"x","reason":"y"}'
```

## Gated tools are unknown, never failed

With the capability off, a privileged or builder-state tool answers
exactly like a typo (verified against the binary):

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"package_bump","arguments":{"package":"x","reason":"y"}}}
```

with `SBM_BUILD=off`:

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"unknown tool or invalid arguments"}}
```

There is no "permission denied" code anywhere in the protocol. Count
the surface instead: default **23**, `SBM_BUILD=off` **21**,
`SBM_SAPHIRA=off` **12**, both off **10** (the journal ten, always).
