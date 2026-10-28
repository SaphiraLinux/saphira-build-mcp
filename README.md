# saphira-build-mcp

Local Model Context Protocol server for the Saphira Linux package and
build environment. Read-only: it answers structured queries from
recipes, APK/repository metadata, the build queue and retained logs,
so agents no longer improvise those investigations by hand.

No build, sign, publish, install or restart surface exists by design.
Missing data sources produce explicit errors, never invented results.

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
`SBM_PASSWD` `SBM_GROUP` `SBM_APKDB`

## Licence

Saphira-owned code under BUSL-1.1 (see LICENSE). Vendored JSON
under MIT, attributed in THIRD_PARTY.md.
