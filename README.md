# saphira-build-mcp

A local Model Context Protocol (MCP) server with two capabilities:

1. **Portable agent recovery** — checkpoints, session handoff,
   durable notes and controlled recall on any supported Linux host.
2. **Saphira build integration** — optional package, recipe,
   repository, build queue and policy tools when `SBM_SAPHIRA=on`.

You do **not** need Saphira Linux, `/recipes`, a Saphira repository
or a builder database to use the portable features — and you do not
need to compile anything. The repository ships six prebuilt
binaries plus one installer. Each platform comes in two sizes:
small (shared system SQLite) and large (`-builtin`, SQLite
compiled in, zero database dependency):

| Small (shared) | Large (builtin) | Platform |
|---|---|---|
| `bin/saphira-build-mcp.osx` | `bin/saphira-build-mcp.osx-builtin` | macOS, Apple Silicon |
| `bin/saphira-build-mcp.musl` | `bin/saphira-build-mcp.musl-builtin` | Linux, musl libc |
| `bin/saphira-build-mcp.arch` | `bin/saphira-build-mcp.arch-builtin` | Linux, glibc |

The `-builtin` binaries carry SQLite inside (the `.musl` one is
fully static); the small ones link the system SQLite — the musl
small binary is ~99 KB against the builtin's ~818 KB. All six are
built from the tagged release named in `SHA256SUMS` and verified
by checksum at install time. Bundling SQLite and producing a
completely static executable are not the same thing, and the
matrix is honest about it: the macOS builtin links only the
system `libSystem` (verified with `otool -L`: no `libsqlite3`
entry, where the small binary lists it), and the Arch builtin
needs only `libc` plus the loader (verified with `ldd`: no
`libsqlite3` entry, where the small binary lists it). Only the
musl builtin is static end to end. The same binaries run on Arch,
Debian, Ubuntu, Alpine, Saphira or another compatible Linux
distribution. (Arch, Saphira and Apple Silicon are test-verified;
the others follow from the same POSIX C and are marked where
proven.)

It never builds, signs, publishes, installs or restarts anything,
and it never executes recipe scripts. Missing data sources produce
explicit errors, never invented results.

## Portable recovery and Saphira builder modes

| Capability | Portable (`off`) | Saphira builder (`on`) |
|---|---|---|
| Checkpoint save/list/resume/close | Yes | Yes |
| Notes journal | Yes | Yes |
| Configured external tools | Yes | Yes |
| Saphira recipes and package queries | No | Yes |
| Saphira build queue and bump | No | Yes |
| Saphira policy/readiness tools | No | Yes |

- `SBM_SAPHIRA=off` enables portable mode.
- `SBM_SAPHIRA=on` or unset preserves builder mode.
- Any other value fails at startup before serving anything.
- `SBM_NOTES_DB` selects the local journal. Recommended for
  ordinary users: `~/.local/share/saphira-build-mcp/notes.db`.
  No mode-dependent path is ever invented.
- Journals are local to each host. Portable mode does **not**
  synchronise across hosts; each machine keeps its own journal.
- Closed checkpoints retain their history and stay resumable by id.

## Quick start: portable Linux host

Prerequisites (build tools, SQLite library plus headers and CLI,
Python 3 for the test suite):

- Arch: `pacman -S gcc make sqlite python` (suite verified on Arch)
- Debian/Ubuntu: `apt install build-essential libsqlite3-dev sqlite3 python3`
- Alpine: `apk add gcc musl-dev make sqlite-dev sqlite python3`
- WSL2: use the Ubuntu instructions inside your distribution

```sh
git clone https://github.com/SaphiraLinux/saphira-build-mcp.git && cd saphira-build-mcp
./install.sh                              # binary + man page only
./install.sh --shared | --builtin         # force SQLite linkage
./install.sh --configure-agents           # plus detected agents
./install.sh --configure-agents --with-rules=FILE
                                          # plus append checkpoint rules
```

The installer detects operating system, architecture and libc,
then picks the variant automatically: shared where the system
provides SQLite, builtin elsewhere (`--shared` / `--builtin`
force either). It checksum-verifies the binary before writing
anything (needs `sha256sum` on Linux or `shasum` on macOS;
without either it refuses outright) and installs under
`~/.local/bin` (root installs to `/usr/bin`; `PREFIX=` and
`DESTDIR=` supported). It refuses to install on a checksum
mismatch and proves the installed copy by running its
`tools/list`. macOS is decided first, so a Linux binary is never
installed on a Mac merely because the
architecture matches.

With `--configure-agents` it additionally wires up whichever
agents are installed: opencode entries are inserted into an
existing `opencode.jsonc` `mcp` block, Codex entries go through
the vendor `codex mcp add` command, and anything unrecognized
falls back to printed manual instructions. Every configuration
write is backup-first (`.bak-sbm-<date>` beside the original);
an entry that already exists is left alone and reported, never
overwritten. `--with-rules=FILE` appends the checkpoint
discipline to a persistent instruction file, skipping when
already present.

To build from source instead:

```sh
make                # static binary where supported, else dynamic
make test           # hermetic suite, no live paths touched
sudo make install   # root: /usr/bin; without sudo: ~/.local/bin
```

macOS compiles and the suite passes on Apple Silicon (verified
on arm64): Xcode command line tools provide compiler, make,
SQLite and `shasum` — no Homebrew packages are required to
build, test or run the MCP binary itself. Two known limits:
sandboxed external tools need `bwrap` (Linux-only — those calls
fail closed without it) while everything else runs) and evidence
digests are computed in-process, so freshness checks work
identically with no `sha256sum` anywhere.

Setting up the *agent* on a Mac is where Homebrew enters. The
MCP binary itself never needs Homebrew — only the software
around it might. Verified on Apple Silicon with Homebrew 7.0.11:

- **Node.js** (only needed by agents built on it): `brew install
  node` (formula verified present; the install itself was not
  exercised — Node was not required for anything tested here).
- **OpenCode:** install from the official installer at
  opencode.ai, then run `./install.sh --configure-agents`.
  (Verified with opencode v2.2.2, which arrived out-of-band: the
  bottled formula still offered 2.2.1 at the time, so prefer the
  official installer.) Confirm with `opencode mcp list`, which
  should show `saphira-build connected`.
- **Codex:** a Homebrew formula exists (verified present, not
  installed here). After installing, wire it with
  `codex mcp add saphira-build --env SBM_SAPHIRA=off [...]`
  as documented under Other agents, or let
  `./install.sh --configure-agents` do it, and confirm with
  `codex mcp list`.

Whichever agent you use, finish with the round-trip proof from
the OpenCode section: save a checkpoint, restart, confirm the
fresh session lists the register before doing anything else.

## Important: install agent checkpoint rules

Installing the MCP does **not** teach your agent when to save,
list, resume or close checkpoints. Without explicit discipline,
agents waste context rediscovering work, querying unrelated
systems, or restarting investigations from scratch.

Ask your coding agent to do this setup (adapt to your own words):

> Read `rules/AGENT_MCP_RULES.md`, install its checkpoint
> discipline into my agent's persistent instructions, configure the
> MCP, and verify that a fresh session lists checkpoints before
> doing anything else. Keep the rules short and preserve my
> existing instructions.

Why it matters: the MCP remembers, but only the rules keep the
agent focused — list first, resume only what the human selects,
never wander. The template is ~230 words on purpose: rules are
loaded into every session's context, so every extra word taxes
every future resume.

`rules/AGENT_MCP_RULES.md` is a repository template. Your
installed rules file is your own persistent configuration: adapt
the template into it, never blindly overwrite it.

## How checkpoint recovery works

A checkpoint is a pre-restart write meant to be consumed
post-restart: label, title, what was done, repository and commit
state, findings, and the next action. Checkpoints form a
persistent register of unfinished work shared by all agents on
the host. `checkpoint_list` shows the active register (newest
first, with a total count); the `initialize` reply advertises the
active set in its `instructions` field. `checkpoint_resume`
consumes one row by id, by label, or the newest overall.
`checkpoint_close` retires finished work without deleting its
history.

The ritual: a new session lists the register and asks the
operator whether to resume one, start fresh, or close one — it
never auto-resumes. Before any restart, long task, or context
compaction, the working agent saves a checkpoint; at meaningful
milestones it updates it; when work finishes it closes it. The
recovery test for every task is mechanical: *could another agent
resume this from the checkpoint plus the repository, without
asking the previous agent what happened?* If not, the handover
was not finished.

A checkpoint is the starting evidence for recall, not permission
to ignore contradictions. If the operator asks for verification,
or an essential fact is missing or demonstrably stale, the agent
inspects only that evidence and reports the difference.

This discipline was built alongside agents doing real release
engineering — queueing builds, diagnosing sandbox failures from
retained logs, recording policy evidence, handing half-finished
work across days and machines. The register, the append-only
journal, and the rules above are what made that work. They are
the product as much as the query tools.

## OpenCode configuration

Working example (replace `USER` with the actual username; paths
must exist as written):

```jsonc
{
  "instructions": [
    "/home/USER/.config/opencode/rules/AGENTS.md"
  ],
  "mcp": {
    "saphira-build": {
      "type": "local",
      "command": ["/home/USER/.local/bin/saphira-build-mcp", "--mcp"],
      "environment": {
        "SBM_SAPHIRA": "off",
        "SBM_AUTHOR": "USER",
        "SBM_NOTES_DB": "/home/USER/.local/share/saphira-build-mcp/notes.db"
      },
      "enabled": true
    }
  }
}
```

This shape is deployed and verified: `opencode mcp list` shows
`saphira-build connected` with exactly these keys (`type`,
`command`, `environment`, `enabled`). Use `/usr/bin/...` as the
command path when installed with sudo instead.

Install the checkpoint rules from the previous section into the
file named in `instructions` (adapting, not overwriting), then
verify end to end: `opencode mcp list` should show
`saphira-build connected`; then save a checkpoint, restart the
agent, and confirm the fresh session lists the register and asks
what to do before anything else. That round-trip — not the config
file alone — is the proof the setup works.

## Other agents

The universal contract is small: any MCP client that can launch a
stdio command with environment variables can use this server.
Command is the installed binary plus `--mcp`; environment is
`SBM_SAPHIRA=off`, `SBM_AUTHOR=<user>` and
`SBM_NOTES_DB=<per-host journal path>`; no ports, no network, no
working-directory requirements.

**Codex** (verified against the real CLI): entries live in
`~/.codex/config.toml` under `[mcp_servers.<name>]`, or via the
vendor command, which is what `./install.sh --configure-agents`
uses:

```sh
codex mcp add saphira-build \
  --env SBM_SAPHIRA=off --env SBM_AUTHOR=USER \
  --env SBM_NOTES_DB=/home/USER/.local/share/saphira-build-mcp/notes.db \
  -- /usr/bin/saphira-build-mcp --mcp
```

Confirm with `codex mcp list` (connected) and `codex mcp get
saphira-build` (shows command, args and masked env). Codex loads
persistent instructions from `AGENTS.md` files — append
`rules/AGENT_MCP_RULES.md` to yours (the installer does this
with `--with-rules=FILE`) so checkpoint discipline survives new
sessions.

**Claude Code** (shape per Anthropic's docs — no Claude host was
available to verify against, so confirm locally before trusting
it): from the project directory,

```sh
claude mcp add saphira-build --transport stdio \
  --env SBM_SAPHIRA=off --env SBM_AUTHOR=USER \
  --env SBM_NOTES_DB=/home/USER/.local/share/saphira-build-mcp/notes.db \
  -- /usr/local/bin/saphira-build-mcp --mcp
claude mcp list
```

or equivalently a `.mcp.json` entry with `command`, `args` and
`env` keys. Persistent instructions live in `CLAUDE.md`; install
the rules template there the same way.

**Anything else:** configure the binary plus the three
environment variables through that client's stdio-server
mechanism, install the rules template into its persistent
instructions, and run the same round-trip proof (save, restart,
list-first). Client formats we have not tested are not documented
here — the stdio contract above is the whole interface.

## Checkpoints and local journal

The journal is append-only: new rows supersede, nothing is
updated or deleted, and closed checkpoints keep their history.
Its tables are maintained by this program. Back it up with a
SQLite-safe snapshot (`sqlite3 notes.db .backup snapshot.db`,
which uses the online backup API) and transfer the finished
snapshot — never copy the live file while writes may occur, and
never use Git for journal sync.

Attribution defaults to `$SBM_AUTHOR` (set it once in the MCP
configuration); the recorder field is client metadata —
client-reported and unverified.

Policy records (builder mode) pin the revisions verified
(recipe, registry and artifact digests). Changed pins report
STALE; absence of a record reports UNVERIFIED. A stale PASS never
reads as valid.

## Saphira builder integration

On a Saphira builder host (default mode) the server additionally
answers structured queries so agents stop improvising them:

- Package discovery and dependency queries across recipes and
  the repository (`package_search`, `package_info`,
  `package_dependencies`, `package_why_installed`,
  `package_installed`). Disabled recipes are excluded unless
  `include_disabled` is set.
- Build queue and retained build/install log tails
  (`build_queue`, `package_build_status`,
  `package_build_history`, `package_build_log`,
  `package_artifacts`).
- Unix identity, readiness and policy evidence
  (`package_identity`, `package_readiness`,
  `package_policy_status`, `package_policy_record`).
- Guarded actions: `build_request` queues through the daemon
  intake (the daemon executes detached — the MCP never runs the
  build itself) and `package_bump` runs the packager's guards,
  with the reason journaled either way.
- Human-controlled signing and publishing: a staged package
  names the `sudo sign-apk-repo` ask instead of performing it.
  Nothing here can sign, publish or install.

Journal attribution and evidence freshness work as described
above; the journal path defaults to the builder location and
`SBM_NOTES_DB` overrides it.

## MCP tools and protocol

```sh
./saphira-build-mcp --mcp         # stdio server (NDJSON primary,
                                  # Content-Length fallback)
./saphira-build-mcp --list-tools  # tools/list document
./saphira-build-mcp --help        # human-readable help, stdout, exit 0
./saphira-build-mcp -h            # same as --help
./saphira-build-mcp --version     # version line, stdout, exit 0
```

`--help` (and `-h`) prints the product name, version, usage,
supported options, purpose and attribution, and exits 0 without
starting the server, opening any database, or emitting protocol
output — it works with no configuration at all. Unknown flags
print usage to stderr and exit 2. Help and branding appear only
on explicit request: MCP protocol output never carries them.

Product identity, as printed by `--help`:

Saphira Linux — https://saphira.vm2.uk. Created by AKADATA
LIMITED for free; the repository LICENSE (Business Source
License 1.1) applies for commercial use — see
https://www.akadata.co.uk. Upcoming: Saphira-MemoryD and the
SHAMPOO product lines (Shared Human-Agent-Model Platform for
Orchestration & Operations) — https://shampoo.op2.uk. Free
distribution does not by itself grant unrestricted commercial
rights; the licence terms govern.

Twenty-two built-ins in builder mode (sixteen read-only plus six
narrowly-scoped actions), six in portable mode (checkpoints and
notes), plus any `/etc/saphira/mcp/tools.conf` externals (see
below). Gated-off tools vanish from `tools/list` and refuse calls
as unknown — never as silent success.

## External tools and safety

`/etc/saphira/mcp/tools.conf` (`SBM_TOOLS_CONF` to override)
declares an exposure allowlist for the built-ins and an extension
registry of curated external commands. Missing or unreadable conf
means built-ins only. Example:

```
expose package_search off
tool repo_status /usr/bin/git read 10 65536 off /recipes -- --no-pager status --short --branch
tool recipe_verify /recipes/saphira-packager/tests/recipe-rules.sh test 120 131072 on /recipes --
```

Grammar is strict and fail-closed: a malformed or unsafe line
disables extensions (built-ins stay fully on). `expose` targets
must be real built-ins and can never switch off `checkpoint_*`.
External entries carry fixed argv only — callers pass no
arguments, no shell exists anywhere on the path (`execve`
direct, scrubbed environment). Class is `read` or `test` (no
write class exists); `test` entries run sandboxed by default
(bwrap, `SBM_BWRAP` to override). The sandbox binds the system
trees it finds and the recipe tree where present, so the same
profile runs on merged-usr systems; without `bwrap` at all,
sandboxed calls fail closed while everything else works.

Safety is deny-by-shape, not by promise: shells, interpreters,
command-executing filters (awk/find/xargs), compilers, and
anything that builds, signs, publishes or installs is
unregistrable, as is any exe, directory or fixed arg naming
MCP-owned writable state (`queue.db`, `repository.db`,
`notes.db`, staged incoming). Every invocation (including
failures) is audited to the journal `tool_runs` table with tool,
argv, agent, exit status and elapsed time. See
`tools.conf.example` for the curated starting set.

## Verified platforms

- **Saphira Linux (musl, x86-64):** hermetic suite green,
  canonical `buildpkg` releases.
- **Arch Linux (glibc, x86-64):** hermetic suite green on the
  same tree.
- **macOS Apple Silicon (arm64):** suite green (1 skip: no
  `bwrap` on macOS, fail-closed by design), plus the end-to-end
  agent recovery test below. Intel macOS is not tested.

### macOS Apple Silicon recovery test (2026-10-29, PASS)

On an Apple Silicon M2 Mac running macOS, with OpenCode v2.2.2
and the prebuilt `.osx` binary in portable mode:

1. In the original session, the operator instructed the agent to
   use MCP checkpoint tools only, without filesystem inspection.
2. The agent called `checkpoint_list`, resumed an existing
   checkpoint, and saved a new checkpoint holding an arbitrary
   personal fact and its meaning (content redacted here) as a
   persistence probe.
3. The session was closed and a fresh OpenCode session started.
4. Asked to recover the fact, the new agent called
   `checkpoint_list`, presented the three active checkpoints, and
   waited for operator selection — no auto-resume.
5. On selection, the agent called `checkpoint_resume` and
   correctly recovered the fact and its meaning.

Measured result: OpenCode reported **6,015 tokens used** for the
recovery interaction. That is the session's reported usage, not
an isolated measurement of MCP retrieval.

No filesystem inspection was performed by the agent during the
demonstrated workflow. Codex, other agents, Intel macOS and
every installer configuration were not part of this test and
are not claimed.

## Build, test and packaging

```sh
make                # static binary where supported
make STATIC=0       # dynamic build (Saphira, Arch, Debian ...)
make test           # hermetic suite, no live paths touched
sudo make install   # root: /usr/bin; user: ~/.local/bin
```

`make test` runs hermetic `tests/test.sh`: fixture recipe tree,
queue/repository databases, retained logs, fake `apk`,
`saphira-build` and `bumppkg` doubles on PATH, a scratch journal
database, and scripted JSON-RPC sessions. 252 checks, including
SQLite discipline proof (repository DB byte-identical
throughout; queue deltas and journal growth asserted exactly
where the action tools run), the tools.conf extension suite
(registry, allowlist, deny-lists, timeout, output caps, sandbox
fail-closed, audit rows), and the portable-mode suite (tool
partition, call refusal, cross-process checkpoint recovery,
invalid-mode failure, journal self-initialisation). Suite green
on Saphira (musl), Arch (glibc) and Apple Silicon (see Verified
platforms); WSL2 status as noted in Quick start.

Canonical Saphira builds pull a release tarball and compile under
`/build` as part of a valid Saphira recipe (`buildpkg`,
Bubblewrap sandbox, scratch under `SAPHIRA_TMPDIR`). Nothing in
this repository builds on GitHub; there are no workflows, and
none are wanted.

## Licence

Saphira-owned code under BUSL-1.1 (see LICENSE). Vendored JSON
under MIT, attributed in THIRD_PARTY.md.

---

Happy recall to all humans and agents — not quite total recall.
(Total Recall, 1990: memories bought off a shelf and implanted
wholesale, no receipts, no refunds. This is the opposite: every
record here has an author, a timestamp, and a source you can go
and check. Write it down, pin the revision, hand it over.)

— Saphira Linux, the friendly dragon
