# saphira-build-mcp

Local Model Context Protocol server for the Saphira Linux package and
build environment. It answers structured queries from recipes,
APK/repository metadata, the build queue and retained logs, so agents
no longer improvise those investigations by hand — and it keeps an
Egg-local evidence journal (package notes and policy records with
freshness pins), so the next agent inherits verified findings
instead of rediscovering them.

No signing, publication, installation or restart surface exists by
design: builds queue for the daemon, bumps carry bumppkg's guards,
and a staged package names the human-gated `sudo sign-apk-repo`
ask instead of performing it.
Missing data sources produce explicit errors, never invented results.

## Build and test

```sh
make STATIC=0   # dynamic musl build (static sqlite unavailable here)
make test STATIC=0
```

`make test` runs hermetic `tests/test.sh`: fixture recipe tree,
queue/repository databases, retained logs, fake `apk`, `saphira-build`
and `bumppkg` doubles on PATH, a scratch journal database, and
scripted JSON-RPC sessions. 210 checks, including SQLite discipline
proof (repository DB byte-identical throughout; queue deltas and
journal growth asserted exactly where the action tools run).

## Protocol

```sh
./saphira-build-mcp --mcp         # stdio server (NDJSON primary,
                                  # Content-Length fallback)
./saphira-build-mcp --list-tools  # tools/list document
```

Sixteen read-only tools: `package_search`, `package_info`,
`package_dependencies`, `package_why_installed`, `package_installed`,
`package_build_status`, `package_build_history`, `package_build_log`,
`package_artifacts`, `build_queue`, `package_identity`,
`package_notes_get`, `package_policy_status`, `package_readiness`,
`checkpoint_resume`, `checkpoint_list`.
Six take narrowly-scoped action: `package_notes_add`,
`package_policy_record`, `checkpoint_save` and `checkpoint_close`
write only to the journal; `build_request`
queues through the daemon intake (the daemon executes detached —
the MCP never runs buildpkg itself); `package_bump` runs the
bumppkg guards. Nothing here can sign, publish or install.

Checkpoints are a persistent register of unfinished work shared by
all agents on the host. The `initialize` reply advertises the active
set (first five plus the total count) in its `instructions` field;
`checkpoint_list` shows the active register, `checkpoint_resume`
consumes one row by id, and `checkpoint_close` retires finished work
without deleting its history. A new agent lists the register and asks
the operator whether to resume one, start fresh, or close one — it
never auto-resumes.

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
scripted JSON-RPC sessions. 210 checks, including SQLite read-only
proof (fixture DB hashes byte-identical before and after) and the
tools.conf extension suite (registry, allowlist, deny-lists, timeout,
output caps, sandbox fail-closed, audit rows).

## Protocol

```sh
./saphira-build-mcp --mcp         # stdio server (NDJSON primary,
                                  # Content-Length fallback)
./saphira-build-mcp --list-tools  # tools/list document
```

Twenty-two tools: `package_search`, `package_info`,
`package_dependencies`, `package_why_installed`, `package_installed`,
`package_build_status`, `package_build_history`, `package_build_log`,
`package_artifacts`, `build_queue`, `package_identity`,
plus the journal and checkpoint tools documented above —
and any `/etc/saphira/mcp/tools.conf` externals (see below).

Disabled recipes are excluded unless `include_disabled` is set.

## tools.conf: extensions without recompiling

`/etc/saphira/mcp/tools.conf` (`SBM_TOOLS_CONF` to override) declares
two things: an exposure allowlist for the built-ins, and an extension
registry of curated external commands. Missing or unreadable conf
means built-ins only — today's behaviour, byte-identical.

```
expose package_search off
tool repo_status /usr/bin/git read 10 65536 off /recipes -- --no-pager status --short --branch
tool recipe_verify /recipes/saphira-packager/tests/recipe-rules.sh test 120 131072 on /recipes --
```

Grammar is strict and fail-closed: a malformed or unsafe line disables
extensions (built-ins stay fully on). `expose` targets must be real
built-ins and can never switch off `checkpoint_*`. External entries
carry fixed argv only — callers pass no arguments, no shell exists
anywhere on the path (`execve` direct, scrubbed environment). Class
is `read` or `test` (no write class exists); `test` entries run
sandboxed by default (bwrap, `SBM_BWRAP` to override).

Safety is deny-by-shape, not by promise: shells, interpreters,
command-executing filters (awk/find/xargs), compilers, and anything
that builds, signs, publishes or installs is unregistrable, as is any
exe, directory or fixed arg naming MCP-owned writable state
(`queue.db`, `repository.db`, `notes.db`, staged incoming). An awk
script can be valid and still execute commands, so 0.8.0 ships
narrowly-defined commands only — no generic runner claims. Every
invocation (including failures) is audited to the journal
`tool_runs` table with tool, argv, agent, exit status and elapsed
time. See `tools.conf.example` for the curated starting set.

## Environment

Live paths default to the Saphira system; point them at fixtures
for testing:

`SBM_RECIPES` `SBM_QUEUE_DB` `SBM_REPO_DIR` `SBM_INCOMING` `SBM_TSV`
`SBM_PASSWD` `SBM_GROUP` `SBM_APKDB` `SBM_NOTES_DB` `SBM_TOOLS_CONF`
`SBM_BWRAP` (`SBM_AUTHOR` for journal attribution)

## Canonical builds

Release tarballs here are source snapshots only. Canonical builds
pull the tarball and compile under `/build` as part of a valid
Saphira recipe (`buildpkg`, Bubblewrap sandbox, scratch under
`SAPHIRA_TMPDIR`). Nothing in this repository builds on GitHub;
there are no workflows, and none are wanted.

## Licence

Saphira-owned code under BUSL-1.1 (see LICENSE). Vendored JSON
under MIT, attributed in THIRD_PARTY.md.
