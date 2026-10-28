# saphira-build-mcp

Local Model Context Protocol server for the Saphira Linux package and
build environment. It answers structured queries from recipes,
APK/repository metadata, the build queue and retained logs, so agents
no longer improvise those investigations by hand — and it keeps an
Egg-local evidence journal (package notes and policy records with
freshness pins), so the next agent inherits verified findings
instead of rediscovering them.

No build, sign, publish, install or restart surface exists by design.
Missing data sources produce explicit errors, never invented results.

## Build and test

```sh
make STATIC=0   # dynamic musl build (static sqlite unavailable here)
make test STATIC=0
```

`make test` runs hermetic `tests/test.sh`: fixture recipe tree,
queue/repository databases, retained logs, a fake `apk` on PATH, a
scratch journal database, and scripted JSON-RPC sessions. 79 checks,
including SQLite read-only proof (authoritative fixture DB hashes
byte-identical before and after journal writes).

## Protocol

```sh
./saphira-build-mcp --mcp         # stdio server (NDJSON primary,
                                  # Content-Length fallback)
./saphira-build-mcp --list-tools  # tools/list document
```

Sixteen tools. Fourteen read-only: `package_search`, `package_info`,
`package_dependencies`, `package_why_installed`, `package_installed`,
`package_build_status`, `package_build_history`, `package_build_log`,
`package_artifacts`, `build_queue`, `package_identity`,
`package_notes_get`, `package_policy_status`, `package_readiness`.
Two write only to the journal: `package_notes_add`,
`package_policy_record`.

Disabled recipes are excluded unless `include_disabled` is set.

Policy records pin the revisions verified (recipe, registry and
artifact digests). Changed pins report STALE; missing records report
UNVERIFIED. A stale PASS never reads as valid.

## Journal

The journal is Egg-local (`/var/lib/saphira-build/notes.db` by
default, `SBM_NOTES_DB` to override) and append-only; its tables are
maintained by this program and it must never be carried in `/recipes`
except blank and empty. Keeper/Homer are backup destinations only:
snapshot with `sqlite3 notes.db .backup snapshot.db` (online backup
API) and transfer the finished snapshot — never copy the live file,
never use Git for journal sync.

Attribution defaults to `$SBM_AUTHOR` (set it once in
`opencode.jsonc`, e.g. `"environment": {"SBM_AUTHOR": "muse"}`);
the recorder field is MCP clientInfo metadata — client-reported and
unverified.

## Build and test

```sh
make STATIC=0   # dynamic musl build (static sqlite unavailable here)
make test STATIC=0
```

`make test` runs hermetic `tests/test.sh`: fixture recipe tree,
queue/repository databases, retained logs, a fake `apk` on PATH, and
scripted JSON-RPC sessions. 51 checks, including SQLite read-only
proof (fixture DB hashes byte-identical before and after).

## Protocol

```sh
./saphira-build-mcp --mcp         # stdio server (NDJSON primary,
                                  # Content-Length fallback)
./saphira-build-mcp --list-tools  # tools/list document
```

Eleven tools: `package_search`, `package_info`,
`package_dependencies`, `package_why_installed`, `package_installed`,
`package_build_status`, `package_build_history`, `package_build_log`,
`package_artifacts`, `build_queue`, `package_identity`.

Disabled recipes are excluded unless `include_disabled` is set.

## Environment

Live paths default to the Saphira system; point them at fixtures
for testing:

`SBM_RECIPES` `SBM_QUEUE_DB` `SBM_REPO_DIR` `SBM_INCOMING` `SBM_TSV`
`SBM_PASSWD` `SBM_GROUP` `SBM_APKDB` `SBM_NOTES_DB` (`SBM_AUTHOR`
for journal attribution)

## Licence

Saphira-owned code under BUSL-1.1 (see LICENSE). Vendored JSON
under MIT, attributed in THIRD_PARTY.md.
