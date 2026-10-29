# External tools: how `tools.conf` works (v0.10.8)

Source of truth: `src/conf.c` (`conf_load`), `src/runner.c`, and one
verified end-to-end run (a `codex exec review` that found a real
planted bug — transcript below). `tools.conf.example` is the curated
starting set.

## What it is

`tools.conf` registers **fixed-argv local executables** as extra MCP
tools, and subtracts built-ins per host. It is the operator's
extension surface: site-specific read/test commands (git porcelain,
linters, agent CLIs) become callable tools with no code change.

- Path: `SBM_TOOLS_CONF`, default `/etc/saphira/mcp/tools.conf`.
- **Absent or unreadable file: built-ins only**, byte-for-byte today's
  behaviour. Portable hosts need no conf at all.
- A **malformed or unsafe line disables all extensions** (and all
  `expose` subtractions); built-ins stay fully on. Fail-closed, never
  half-loaded.
- Directives (whitespace-separated, `#` starts a comment at line start
  or after whitespace):

```
expose <name> on|off
tool <name> <abspath> <read|test> <timeout-s> <max-bytes> <sandbox on|off> <dir> -- <fixed args...>
```

## The `tool` line, field by field

`--` must be exactly the 9th token (8 fields, then `--`, then fixed args):

| Field | Rule (enforced, fail line) |
|---|---|
| `name` | lowercase start, `[a-z0-9_]` only, ≤ 32 chars; must not collide with a built-in or another external |
| `exe` | absolute path, no `..`, no shell metacharacters (`;|&$"'\*?~()<>`, backslash, newline); basename must not be denied (below) |
| `class` | `read` (runs unsandboxed) or `test` (runs sandboxed) |
| `timeout-s` | 1–3600; breach → SIGKILL, output gains a `[timeout]` marker |
| `max-bytes` | 1–1048576; combined stdout+stderr capped here |
| `sandbox` | `on` (bwrap) or `off`; stated explicitly either way |
| `dir` | absolute working directory, no `..`, no metacharacters, no denied substrings |
| fixed args | after `--`; **callers pass no arguments, ever** — what is configured is what runs |

`expose <name> off` removes a non-checkpoint built-in from list and
call (unknown, per the gating rule). `checkpoint_*` can never be
switched off. `expose ... on` is accepted and is a no-op record.

## What can never be registered

Three denylists, all fail the line (executed code, `src/conf.c`):

- **Executables** (by basename, with `python3.12`-style versioned
  matching for `python`/`perl`): shells and interpreters (`sh`,
  `bash`, `python*`, `perl*`, `ruby`, `lua`, `php`, `node`, `deno`
  ...), command-running filters (`awk`, `find`, `xargs`, `parallel`
  ...), privilege tools (`env`, `su`, `sudo`, `doas`), the whole
  build/sign/publish/install lane (`apk`, `buildpkg*`, `bumppkg`,
  `makepkg`, `resolvepkg`, `sign-apk-repo`, `installpkg`), namespace
  escapes (`chroot`, `nsenter`, `unshare`, `bwrap`), network tools
  (`ssh`, `scp`, `curl`, `wget`, `nc`, `socat` ...), editors
  (`vi`, `emacs` ...), file mutators (`tee`, `dd`, `cp`, `mv`, `rm`,
  `chmod`, `chown` ...), and compilers (`cc`, `gcc`, `make`, `cmake`,
  `go`, `cargo` ...).
- **State substrings** (anywhere in exe, dir or fixed args):
  `queue.db`, `repository.db`, `notes.db`, `stage4/incoming`,
  `/out/`, `.buildpkg` — the runner must never route around
  MCP-owned build and package state.
- **Agents notes**: `codex` and `opencode` are *not* denied — they are
  single-purpose CLIs, and the fixed-argv + no-shell + scrubbed-env
  design is exactly what contains them (see the verified example).

## How execution works (`src/runner.c`)

- **No shell anywhere**: `execve` with the configured argv, no `PATH`
  search. What the line says is the syscall.
- **Scrubbed environment**: the child gets exactly `PATH`,
  `PAGER=cat`, `GIT_PAGER=cat`, `GIT_CONFIG_NOSYSTEM=1` — no `HOME`,
  no keys, no inherited secrets. An external that needs credentials
  must resolve them without env (see the codex finding below) or not
  run as an external.
- `sandbox off`: `chdir(dir)`, run. `sandbox on`: bubblewrap with
  `--unshare-net` (**no network**), the tool dir read-bound, then run.
  Network-needing externals must be class `read` + `sandbox off`;
  the class documents the operator's intent.
- Combined stdout+stderr (test failures stay visible in one place),
  capped at `max-bytes`; the response appends an
  `[exit=N elapsed_ms=M]` footer. Non-zero exit → `isError: true`.
- Externals list **after** built-ins, in conf order, and obey the same
  envelope. They are unswitchable by capability flags (present in
  locked-down mode) but removable per host via `expose <name> off`.

## Verified example: `codex exec review` as an external

`codex exec "prompt"` does **not** fit fixed-argv: the conf splits on
whitespace with no quoting, and codex treats the extra words as a
COMMAND, failing with `unexpected argument`. The conf-compatible shape
is the promptless subcommand — `codex exec review --uncommitted`
(zero prompt words, fully pinned):

```
tool codex_review_uncommitted /usr/bin/codex read 300 32768 off /home/smalley/worktmp/codereview-fixture -- exec --ephemeral --sandbox read-only review --uncommitted
```

Executed through the MCP server against a fixture repo carrying one
planted buggy function. Result (`isError: false`, `[exit=0
elapsed_ms=36152]`, trimmed):

```
OpenAI Codex v0.163.0 [...] model: gpt-5.6-terra [...] sandbox: read-only [...]
- [P1] Avoid returning a pointer to stack storage — math.c:2-2
  `b` ceases to exist when `greet` returns [...]
- [P1] Bound the greeting formatting output — math.c:2-2
  For any name longer than four characters, `sprintf` writes past the 8-byte `b` buffer [...]
- [P2] Include the declaration for sprintf — math.c:2-2 [...]
```

Three findings locked onto:

```c
char *greet(char *n) { char b[8]; sprintf(b, "hi %s", n); return b; }
```

stack use-after-return, buffer overflow on ordinary inputs, missing
`<stdio.h>` — the exact bug classes a review external exists to catch.

Two things this run proved beyond the transcript:

1. **Auth works under the scrubbed environment.** Codex resolved its
   credentials with no `HOME` and no inherited env (file-based auth
   via the passwd entry on the test host). If *your* agent CLI needs
   env-carried keys, it cannot receive them through an external —
   bake them into a root-owned helper the conf pins, or keep that
   delegation on the agent platform side.
2. **One invocation, one task, one result.** `--ephemeral` leaves no
   session history; `--sandbox read-only` holds codex itself to
   read-only; stdout carries the answer, progress stays on stderr
   (captured into the same text by the runner). From OpenCode or any
   MCP client it is a single `tools/call`.

For interactive or custom-prompt Codex work, keep using the Codex
platform (`codex exec "Review the current Git changes for bugs. Do not
modify files. Report findings only."`, `--sandbox read-only`,
`--ephemeral` for disposable runs). The conf form is for the
repeatable, promptless operations: reviews, fixed audits, verdicts.

## Operator checklist

1. Copy `tools.conf.example` to `SBM_TOOLS_CONF` (root-owned, `0640`).
2. Keep one line per operation; bake scope into fixed args (`--uncommitted`,
   `--no-pager`, paths) — callers cannot add any.
3. `read` + `sandbox off` for network tools (codex); `test` +
   `sandbox on` for local verifiers (recipe rules, linters).
4. Reload = restart the server process (conf is read at startup; the
   tool set never changes mid-process, `listChanged: false`).
5. A refused line fails the whole extension set safe (built-ins only) —
   check `--list-tools` after every edit.
