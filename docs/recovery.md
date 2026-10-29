# Recovery: checkpoints, notes, total_recall (v0.10.8)

The journal (`SBM_NOTES_DB`, a SQLite file) is the continuity plane:
git preserves the source, markdown preserves the human record, the
journal preserves machine-usable state. Every response ends with a
`[src: ...]` footer naming the provenance surface.

## Checkpoints

- `checkpoint_save` (label + body required) records state, SHAs,
  outstanding work and next action. Returns `recorded checkpoint N
  (label '...')`. Checkpoints are **active by default**.
- `checkpoint_resume` takes `id` (a bare number), `label` (newest
  match), or nothing (newest overall). The response is the handoff
  pack: timestamp, author, `recorder=name/version`, a `where:` line
  (`host as user in cwd (server|client origin)`), scope, status and
  the full body. Closed checkpoints stay resumable **by id**.
- `checkpoint_list` shows the active register newest-first with
  `N total (showing M)`; `status`/`include_closed` reach history.
- `checkpoint_close` retires by `id`. **History is preserved, never
  deleted** — closing removes from the active register only.

Rule agents live by: on session start, list and ask whether to resume
one, start fresh, or close one — never auto-resume. (This text ships
in the `initialize` instructions.)

## Notes

`package_notes_add` kinds: `finding`, `decision`, `issue`, `success`,
`failure`, `lesson`. Title optional, body required; the recorder,
timestamp and author are attached server-side. `package_notes_get`
reads newest-first with `kind`/`limit` filters.

## Policy evidence and readiness

Five checks: `unix-identity`, `payload-layout`,
`revision-discipline`, `dependency-closure`, `signing-acceptance`.
`package_policy_record` attaches one PASS/FAIL with an `evidence`
object and supersedes the prior record for that check (record N,
"first record" for the first). `package_policy_status` shows all five
with CURRENT/STALE/UNVERIFIED freshness;
`package_readiness` rolls up verified / stale / unverified plus open
issues and recent notes. Nothing here builds, signs or publishes —
it is journal only.

## total_recall

Three modes over one store (checkpoints, notes, policy records, tool
runs, session events; FTS5-ranked, with a LIKE fallback):

- `query`: ranked retrieval for a `query` string.
- `timeline`: chronological replay, optionally scoped by
  `session_id`/`package`/`label`/`project`/`author`/`hostname`/
  `username`/`cwd`.
- `export`: paginated machine dump with `cursor` (`o<offset>` plus
  creation/table/id cursors).

Knobs: `detail` (`summary`/`full`), `budget` (`compact` 4 KiB,
`standard` 16 KiB, `extended` 64 KiB, `deep` 256 KiB), `limit`
(clamped to 100). Every response states its budget, row count, scope,
the searched surfaces, and the load-bearing exclusion:

```
not searched: tool outputs and conversation turns (never captured)
```

Checkpoints restore working state cheaply; `total_recall` is for when
deeper evidence is required, never automatic.

## Provenance on every write

Journal writes record **where** (hostname, username, cwd —
server-observed, or the client's own context when it supplies one),
**who** (`SBM_AUTHOR` default, per-call `author` override,
`recorder` from `initialize` clientInfo), and **when**. Overrides
exist for hermetic tests (`SBM_HOSTNAME`, `SBM_USERNAME`, `SBM_CWD`);
production carries live values. The `where:` line in resume output
and the `environments (N)` block in recall output make multi-host,
multi-agent recovery unambiguous: a fresh agent sees which host, which
user, which directory, and which session each fact came from.

## Cross-agent pattern

1. Agent A: `checkpoint_save` with state + SHAs + next action.
2. Agent B (any host, any platform): `checkpoint_list`, then
   `checkpoint_resume` by label, then `total_recall` scoped by the
   session for anything the handoff pack elided.
3. Both: `package_notes_add` findings as they occur (never
   end-of-session only), `package_policy_record` for verified claims.

The Windows build shares the same journal file and the same bytes
(see `docs/windows.md`): provenance, not platform, is the identity.
