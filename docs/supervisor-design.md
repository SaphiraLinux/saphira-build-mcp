# Portable agent supervisor: design (review-gated, no implementation yet)

Status: PROPOSED. Nothing here is built. Implementation starts only
after the portability contract and task-supervision details below
are agreed.

Spine: **the agent owns completing the task; MCP is tool access;
A2A is delegation and exchange; the supervisor outlives every
session** — Muse gone home, terminal closed, another session taking
over. Every section below serves that.

## 1. Portability contract (non-negotiable)

The supervisor and `saphira-build-mcp` run on ordinary Linux with no
Saphira Linux, no `buildpkg`, no `saphira-build`, no Saphira
repositories, and no memoryd. Build requirements stay POSIX C plus
SQLite (shared or amalgamated), nothing else.

Capability matrix (fail-closed, explicit — never silent):

| Capability present | Behaviour |
|---|---|
| None of the above | Full generic supervision: exec/workflow/service/cron/codex/opencode/external task types, unix-socket A2A, MCP client tools |
| Saphira builder adapter configured | Adds Saphira build observation/delegation; generic tasks unaffected |
| memoryd reachable | Foresight consumes mistakes/lessons from it; otherwise file evidence only (see §7) |
| bwrap present | Sandbox enforced where configured; absent → `enforced=NONE`, reported (see §8) |

Portable paths. Installed default keeps the Saphira tree
(`/etc/saphira/agent/*.conf`, state `/var/lib/saphira-agent/`,
socket `/var/run/saphira-agent/agent.sock` — packaged-daemon
runtime placement, not the `/run` boot tree). Overrides, honoured
everywhere including unprivileged development (no root, no system
dirs needed):

- `SAGENT_CONF_DIR` (default `/etc/saphira/agent`)
- `SAGENT_STATE_DIR` (default `/var/lib/saphira-agent`)
- `SAGENT_SOCK_DIR` (default `/var/run/saphira-agent`)
- `SBM_*` overrides in the MCP keep working as today.

No hidden directories, no new top-level roots, no network listener
unless explicitly configured (§5).

## 2. Task engine (transport-independent core)

One engine, three frontends: the unix socket (primary), MCP tools
(thin client), and the optional TCP listener (§5). Same state
machine, same records — no frontend may diverge.

Lifecycle (A2A v1.0 TaskState subset):
`submitted → working → completed | failed | canceled`,
with `input-required` (waiting on declared input) and `rejected`
(operator/config refusal) as non-terminal holds. Terminal states are
immutable: a refinement or retry is a new task in the same
`contextId`, never a reopen. Unknown task id → `TaskNotFoundError`;
message to a terminal task → `UnsupportedOperationError`.

- **Submit returns an id immediately.** The child is reparented under
  the supervisor (setsid; no ties to the requesting session, MCP
  process, or terminal). The requester may disconnect at once.
- **Progress without blocking:** append-only `task_events` rows;
  completion, failure and artifacts polled later by any session via
  `agent_task_status` / `agent_task_results` (MCP) or
  `GetTask`/`ListTasks` (A2A). Streaming (`SendStreamingMessage`,
  SSE) is a read of that same log, never a second channel.
- **Completion criteria are declared, not hoped:** each agent file
  states `complete-on` (exit code plus required artifact markers,
  e.g. last-message file present and non-empty). Exit 0 without the
  artifacts is `failed (incomplete)`, recorded as such.
- **One task per invocation.** Each task gets its own process (or its
  own supervised session for service jobs). No multiplexing, no
  shared worker state.
- Supervisor duties, owned end to end: process ownership,
  termination (timeout → SIGKILL + marker), cancellation (flag +
  kill + terminal record), bounded retries with backoff, per-agent
  concurrency slots, recovery after interruption, durable completion
  records.

`buildpkg` spawn vs task delegation (required distinction):

| | build spawn (`build_request`, existing) | Task delegation (this design) |
|---|---|---|
| Owner of outcome | Builder daemon queue; MCP only observes | Supervisor tracks to declared completion |
| Return | Queue row id; fire-and-observe | Task id; status/artifacts queryable to completion |
| Retry/completion | Queue semantics | `complete-on`, retries, artifact checks |
| Relationship | A delegation *may use* `build_request` internally and poll the queue — the completion guarantee belongs to the task, not the queue |

## 3. Configuration contract (`SAGENT_CONF_DIR/*.conf`)

Strict line grammar in the `tools.conf` tradition. Multiple named
files; **per-file fail-closed** (a malformed file disables only that
file's agents — full-directory invalidation would let one bad file
deny all agents). Root-owned 0640 when system-installed; plain files
under the dev override otherwise. Documented contract ships as this
file plus a man page; unknown directives fail the file.

Directives (final names agreed here, parser later):

- `agent <name> type <exec|workflow|service|cron|codex|opencode|external>`
- `skill <text>` — capability advertisement; feeds the Agent Card.
- `exec <abspath> -- <fixed args…>` — the only command surface.
- `task-input <stdin-file|file-arg <flag>|none>` — how the single
  assigned task reaches the child (§6). `none` = fixed argv only.
- `param <name> <validator>` — declared task parameters only
  (`recipe-name` must exist in the tree, `path` confined under a
  prefix, closed enums). Undeclared input is refused.
- `permit <capability>` — narrow authority allowlist, default deny.
- `concurrency N`, `retries N`, `timeout-s N`,
  `complete-on <exit-code + artifact markers>`.
- `schedule <spec>` — supervisor-owned schedule for `cron` type.
  Never touches system cron.
- `sandbox on|off` — explicit operator choice per agent (§8).
- `codex-profile` / `opencode-profile` — per-type fixed flags
  (`--sandbox`, `--model`, `--agent`, …); values are operator-fixed,
  never caller-supplied.

No caller-controlled arbitrary execution, ever: the command and its
flags are operator-fixed; parameter *values* pass strict validators;
free text travels only by task-file mechanisms (`stdin-file`,
`file-arg`), never argv.

## 4. Storage and recovery (the spine)

- `tasks.db` (SQLite, supervisor-owned, in `SAGENT_STATE_DIR`):
  `agent_tasks` (id, context, agent, type, state, params digest,
  pid, proc start identity, declared completion, attempts),
  `task_events` (append-only progress), `task_artifacts`
  (file-backed results + digests), `schedules`.
- Every spawn records **process identity**: pid plus Linux
  `/proc/<pid>/stat` starttime (field 22) and the credential check
  below. On supervisor restart the recovery scan compares recorded
  identity against live `/proc`:
  - pid alive **and** starttime **and** UID match → reattached
    (`working`, ownership resumed);
  - anything else → `interrupted (supervisor restart, identity
    unverified)`, retryable per policy, never silently resumed.
- **Never claim a reattach without verification.** Where `/proc` is
  unavailable (non-Linux dev), the portable rule is stricter, not
  looser: no reattach claims at all — live children are inventoried
  by supervisor-held handles while running, and anything
  unaccounted for after a restart is marked interrupted.
- Socket peers are authenticated by filesystem permissions plus
  `SO_PEERCRED` (uid/pid of the submitter recorded on the task).
  Client death changes nothing: tasks have no handle on the
  requester.

## 5. Transports: local first, standards-shaped

- **Primary: unix socket** (`agent.sock` in `SAGENT_SOCK_DIR`),
  JSON-RPC with the A2A v1.0 method surface as the shape:
  `SendMessage` (honours `ReturnImmediately` — fire-and-forget
  returns the task id at once), `SendStreamingMessage` (SSE over
  the socket log), `GetTask`, `ListTasks`, `CancelTask`,
  `SubscribeToTask`, plus the well-known Agent Card document
  (served from config; `/.well-known/agent-card.json` shape when
  the TCP binding is enabled).
- **MCP tools** (in `saphira-build-mcp`, thin client over the
  socket, additive only): `agent_list` (discovery/cards),
  `agent_task_submit` (returns id immediately),
  `agent_task_status`, `agent_task_cancel`, `agent_task_results`.
  No task executes in the MCP process.
- **TCP: explicitly configured or absent.** No listener code runs
  unless a `listen` stanza names address, port, and a bearer-token
  file. Default bind when enabled is loopback; a non-loopback bind
  requires a second explicit flag. Auth requirements appear on the
  Agent Card exactly as configured. Interop-first, exposure-last.

## 6. Execution adapters (verified CLI contracts, not assumptions)

Codex and OpenCode were checked against their real CLIs (docs and
sources, Nov 2026) and they do **not** accept the same task input —
so each type has its own fixed `task-input` strategy:

- **codex** (`task-input stdin-file`): `codex exec -` with stdin
  redirected from the supervisor-written task file (stdin *is* the
  prompt; the `-` form). Fixed flags per profile (`--sandbox`,
  `--json`, `-o <artifacts>/last-message.md`,
  `--skip-git-repo-check`, `-C <dir>`). Stdout carries the final
  message, stderr the progress log, exit code plus `-o` file the
  completion evidence. **Stdin is never inherited**: an open,
  TTY-less stdin hangs `codex exec` on an unbounded read (observed
  upstream) — the supervisor always redirects from `/dev/null` or
  the task file.
- **opencode** (`task-input file-arg -f`): `opencode run --format
  json [--agent …] [--model …] -f <taskfile> "<fixed short
  instruction>"`. OpenCode takes its message from argv and has no
  stdin-prompt form, so the task travels as a `-f` file attachment
  at a fixed position; the path is supervisor-generated under the
  task dir (confined, deterministic). Stdin from `/dev/null`.
- **exec**: fixed argv; validated `param` values only.
- **workflow**: ordered fixed-argv stages with artifact passing.
- **service**: supervisor-spawned long-running child with restart
  policy. Explicitly *not* system-service control — service
  restarts stay operator-only; read-only state queries only if
  permitted.
- **cron**: fires from the supervisor's own schedule table.
- **external**: another configured AI or non-AI application CLI,
  same file mechanisms, profile-fixed flags.

## 7. Foresight (portable rule generator, not a store)

- **Evidence in, rules out.** Foresight consumes mistakes and
  lessons from memoryd when reachable, and accepts independent
  evidence (task records, build logs, files) when it is not. It
  creates **no competing knowledge store**: candidates and their
  evidence links live where the evidence lives (memoryd) or ride
  with the task record that produced them.
- **Rule lifecycle:** `proposed → validated → enforced → retired`
  (plus `rejected`). Validation is a deterministic replay against
  past failures plus fixtures, evidence attached to the candidate.
- **Enforcement targets are pluggable.** Saphira recipe checks are
  *one* optional target (promoted by a human into builder-owned
  storage, e.g. enforced-checks wired into `recipe-rules.sh`);
  the generic target is an enforced-checks directory plus a CI-hook
  example that any project can adopt. The generator never flips
  enforcement itself, never silently gates anything on an
  unverified rule, and never weakens an established policy.
- Seed corpus (unchanged): `/usr/lib64` in payloads, missing
  `-dev` dependencies, overwritten `CFLAGS`, generated Perl
  collisions, missing service executables — each becomes a
  `proposed` rule with a check sketch once the generator lands.

## 8. Authority, sandbox, and honest confinement

- Config `permit` lines declare policy; the supervisor additionally
  reports **permissions actually enforced by the OS**: every task
  status shows `declared` (permits, sandbox setting) beside
  `observed` (uid, sandbox mechanism active or
  `enforced=NONE (bwrap absent)`, read-only binds, network state).
  A gap between the two is a visible finding, never a silent
  assumption.
- `sandbox on|off` is an explicit operator choice per agent;
  default on for test-class executables. Off is allowed where the
  task legitimately needs the live tree — stated, attributed,
  auditable.
- Reuse of the `tools.conf` denylist model for agent executables
  (no shells/interpreters-as-executors, no privilege or network
  tools, no build/sign/publish/install lane for the Saphira
  adapter).

## 9. Signing and publication (adapter-scoped, not universal)

The existing operator-controlled Saphira workflow is untouched:
signing/publication/installation have no tool surface and no agent
authority grant can create one — those restrictions live in the
**Saphira build adapter**, as denylist plus `permit` refusal. The
generic supervisor carries no such concepts; other deployments
define their own authority model through `permit`, default deny.
Universal assumptions about one project's release gates are not
baked into portable machinery.

## 10. Test plan (portable: no Saphira infrastructure)

Hermetic fixtures only; the suite must pass on a bare Linux host
(and macOS where paths allow) with no builder, no repos, no
memoryd, no bwrap:

- **Contract:** strict-grammar acceptance/rejection per
  directive; per-file fail-closed isolation; denylist refusals;
  root-owned 0640 vs dev-mode files; `param` validator matrix
  (bad recipe-name, escaping path, bad enum refused).
- **Lifecycle:** submit→working→completed/failed/canceled/
  timeout; terminal immutability (refinement = new task);
  `input-required` roundtrip; `complete-on` gating (exit 0
  without artifacts ≠ completion).
- **Persistence (the spine):** `kill -9` supervisor mid-task →
  restart → verified reattach or honest `interrupted`; client
  disconnect survival (submit, kill client, status from a fresh
  session); reboot-equivalent recovery run.
- **Concurrency/retries/cancellation:** slot caps, backoff bounds,
  cancel-then-terminal-record proofs.
- **Authorization:** unknown agent, undeclared param, denied exe,
  sign-shaped task — refused with evidence, never silent.
- **Declared-vs-enforced:** sandbox-off and bwrap-absent cases
  report the gap instead of claiming confinement.
- **A2A conformance:** card shape from config; TaskState
  transition legality incl. terminal rejection;
  artifact/message mapping; socket permission refusal for
  unauthorized uid; `ReturnImmediately` returns at once.
- **Foresight:** derivation links resolve to real evidence;
  attempt to enforce an unvalidated rule is refused by code
  path; enforced check runs green with memoryd down.
- **Compatibility:** existing MCP suite green; `tools/list`
  additive only; builder/dashboard suites untouched and green;
  FHS paths; both init units present and disabled.

## 11. Proposed sequencing (each step review-gated)

1. Portability contract + config contract + `tasks.db` schema
   (this document, agreed first).
2. Engine core: lifecycle, spawn/supervise, recovery scan.
3. Unix-socket A2A surface + Agent Cards.
4. MCP client tools (`agent_*`).
5. `exec` + `codex` + `opencode` adapters (verified contracts
   above), then workflow/service/cron.
6. Foresight generator + pluggable enforcement targets.
7. TCP listener — only on explicit request, default off,
   exposure-last.
