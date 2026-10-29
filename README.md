# saphira-build-mcp

A local Model Context Protocol (MCP) server that gives AI agents a
safe, structured window into a software build and package universe —
and a durable memory to hand work between sessions. It answers
questions from recipes, APK/repository metadata, the build queue and
retained logs, so agents no longer improvise those investigations by
hand. It keeps an evidence journal (package notes and policy records
with freshness pins) and a register of session checkpoints, so the
next agent inherits verified findings instead of rediscovering them.

No signing, publication, installation or restart surface exists by
design: builds queue for the daemon, bumps carry the packager's
guards, and a staged package names the human-gated
`sudo sign-apk-repo` ask instead of performing it.
Missing data sources produce explicit errors, never invented results.

## Why checkpoints exist

Agent context is finite and sessions die: restarts, compaction,
crashes, the human closing the laptop. Without a handover protocol,
everything the previous agent learned dies with it and the next one
starts by interrogating the operator — or worse, by guessing.

A checkpoint is a pre-restart write meant to be consumed
post-restart: label, title, what was done, repository/branch/commit
state, findings, and the next action. Checkpoints form a persistent
register of unfinished work shared by all agents on the host:

- `checkpoint_list` shows the active register (newest first, with a
  total count). The `initialize` reply advertises the active set in
  its `instructions` field, so every session starts aware of it.
- `checkpoint_resume` consumes one row by id, by label (newest), or
  the newest overall. Closed checkpoints stay resumable by id.
- `checkpoint_close` retires finished work from the register without
  deleting its history.

The ritual matters more than the tools: a new agent lists the
register and asks the operator whether to resume one, start fresh,
or close one — it never auto-resumes. Before any restart, long
build, or context-compacting step, the working agent saves a
checkpoint. When work finishes, it closes it. The recovery test for
every task is mechanical: *could another agent resume this from git
plus docs plus the journal, without asking the previous agent what
happened?* If not, the task is not complete.

This server was built alongside agents doing real release
engineering — queueing builds, diagnosing sandbox failures from
retained logs, recording policy evidence, handing a half-finished
port across days and machines. The checkpoint register, the
append-only journal, and the rule below are what made that work.
They are the product as much as the query tools.

## Make your agents use checkpoints by default

If you run opencode (or any MCP-capable agent) with this server,
put a rules block like this in your `AGENTS.md` so handovers happen
without being asked:

```markdown
## Session checkpoints (saphira-build-mcp)

- Session start: call `checkpoint_list`; tell the operator what is
  active and ask whether to resume one, start fresh, or close one.
  Never auto-resume.
- During work: record durable findings as they occur
  (`package_notes_add`, checkpoints) — decisions, provenance,
  next steps. Never wait until context is nearly exhausted.
- Before any restart, compaction, or detached build: `checkpoint_save`
  with a label, what was done, repo/branch/commit state, and the
  next action.
- When work finishes: `checkpoint_close` by id. History is kept;
  the register stays clean.
- A task is NOT DONE until another agent could resume it from git
  plus docs plus the journal alone.
```

How it works in practice: the agent's session ritual starts with a
single `checkpoint_list` call. If the register holds an
`enable-saphira-bucket` checkpoint from Tuesday, Wednesday's agent
resumes it by id and sees the exact source and recipe commits, the
last build verdict, and the outstanding human gate — then carries on
instead of re-asking. Notes taken under a package name (`finding`,
`decision`, `issue`, `success`, `failure`, `lesson`) accumulate into
a per-package engineering log with authors and timestamps; policy
records pin the revisions verified (recipe, registry and artifact
digests), so changed pins report STALE and missing records report
UNVERIFIED. A stale PASS never reads as valid.

## Protocol

```sh
./saphira-build-mcp --mcp         # stdio server (NDJSON primary,
                                  # Content-Length fallback)
./saphira-build-mcp --list-tools  # tools/list document
```

Twenty-two built-ins. Sixteen read-only tools: `package_search`,
`package_info`, `package_dependencies`, `package_why_installed`,
`package_installed`, `package_build_status`,
`package_build_history`, `package_build_log`, `package_artifacts`,
`build_queue`, `package_identity`, `package_notes_get`,
`package_policy_status`, `package_readiness`, `checkpoint_resume`,
`checkpoint_list`. Six take narrowly-scoped action:
`package_notes_add`, `package_policy_record`, `checkpoint_save` and
`checkpoint_close` write only to the journal; `build_request`
queues through the daemon intake (the daemon executes detached —
the MCP never runs the build itself); `package_bump` runs the
packager's guards. Nothing here can sign, publish or install.

Disabled recipes are excluded unless `include_disabled` is set.

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
script can be valid and still execute commands, so narrowly-defined
commands only — no generic runner claims. Every
invocation (including failures) is audited to the journal
`tool_runs` table with tool, argv, agent, exit status and elapsed
time. See `tools.conf.example` for the curated starting set.

## Environment

Live paths default to the Saphira system; point them at fixtures
for testing:

`SBM_RECIPES` `SBM_QUEUE_DB` `SBM_REPO_DIR` `SBM_INCOMING` `SBM_TSV`
`SBM_PASSWD` `SBM_GROUP` `SBM_APKDB` `SBM_NOTES_DB` `SBM_TOOLS_CONF`
`SBM_BWRAP` (`SBM_AUTHOR` for journal attribution)

## Portable mode

`SBM_SAPHIRA=off` runs the same binary on machines without the
Saphira builder: every tool that needs builder state is hidden
from tools/list and refuses calls, while checkpoints, the notes
journal and configured externals keep working. Set `SBM_NOTES_DB`
to a per-host journal path (recommended:
`~/.local/share/saphira-build-mcp/notes.db`); the builder default
is unchanged and no mode-dependent path is invented. Unset or
`on` keeps full behaviour; any other value fails before serving.
`make install` follows privilege: root installs to `/usr/bin`,
an ordinary user to `~/.local/bin` (explicit `PREFIX=` wins).

## Build and test

```sh
make STATIC=0   # dynamic musl build (static sqlite unavailable here)
make test STATIC=0
```

`make test` runs hermetic `tests/test.sh`: fixture recipe tree,
queue/repository databases, retained logs, fake `apk`, `saphira-build`
and `bumppkg` doubles on PATH, a scratch journal database, and
scripted JSON-RPC sessions. 252 checks, including SQLite discipline
proof (repository DB byte-identical throughout; queue deltas and
journal growth asserted exactly where the action tools run), the
tools.conf extension suite (registry, allowlist, deny-lists, timeout,
output caps, sandbox fail-closed, audit rows), and the portable-mode
suite (tool partition, call refusal, cross-process checkpoint
recovery, invalid-mode failure, journal self-initialisation).

## Canonical builds

Release tarballs here are source snapshots only. Canonical builds
pull the tarball and compile under `/build` as part of a valid
Saphira recipe (`buildpkg`, Bubblewrap sandbox, scratch under
`SAPHIRA_TMPDIR`). Nothing in this repository builds on GitHub;
there are no workflows, and none are wanted.

## Licence

Saphira-owned code under BUSL-1.1 (see LICENSE). Vendored JSON
under MIT, attributed in THIRD_PARTY.md.

---

Happy recall to all humans and agents — not quite total recall.
(Total Recall, 1990: memories bought off a shelf and implanted
wholesale, no receipts, no refunds. This is the opposite: every
record here has an author, a timestamp, and a source you can go and
check. Write it down, pin the revision, hand it over.)

— Saphira Linux, the friendly dragon
