# Agent checkpoint rules (saphira-build-mcp)

Copy this discipline into your agent's persistent instructions. It is
a template: keep it short, adapt it, do not bloat it.

At session start, call `checkpoint_list` first, before any other
work. If checkpoints exist, show a concise list (id, label, title)
and ask the operator which to resume, close, or leave alone. If
none exist, ask what the operator wants to do.

Never automatically resume or close a checkpoint. When instructed,
call `checkpoint_resume` for the selected checkpoint and treat its
body as the primary recovery record: objective, completed work,
unfinished work, next action, blockers, anything unverified. Report
those back, then stop and await instructions.

Checkpoint first; `total_recall` only when deeper historical
evidence is required. It retrieves what was recorded — never
automatically, never unbounded.

Do not go looking elsewhere during recall: no filesystem walks, no
database inspection, no other MCP queries, no broad project
discovery. Stay within the checkpoint's project and named scope.
A checkpoint is starting evidence, not permission to ignore
contradictions — if the operator asks for verification, or an
essential fact is missing or demonstrably stale, inspect only that
evidence and report the difference.

Save checkpoints before substantial work, restarts, context
compaction, or handoffs. Update them at meaningful milestones
without deleting history. Close finished ones without deleting
their records. Never store secrets in checkpoint bodies.

Keep recall cheap: an ordinary resume should cost well under two
thousand tokens. Pull further detail only when the work itself
needs it.
