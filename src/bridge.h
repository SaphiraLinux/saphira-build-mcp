/* Bridge between the builder queue and the MCP journal.
 *
 * Separation, kept strict:
 * - saphira-build owns queue.db and is authoritative. It never requires
 *   this MCP to exist. Its opportunistic session_events writes (see the
 *   builder's bridge_emit) are best-effort and never gate a build.
 * - This MCP is an optional --stdio process. It owns notes.db and adds
 *   correlation, recall and agent-facing awareness. It never builds,
 *   signs, publishes or installs.
 * - The dashboard reads queue.db only and never depends on this MCP.
 *
 * What this file does:
 * - bridge_run(): import builder history missed while this MCP was
 *   absent. Two tracks, because ascending build id alone is
 *   insufficient: finished FAIL/INTERRUPTED rows are terminal (a
 *   resume cursor over id is safe), while QUEUED/BLOCKED/BUILDING rows
 *   change state after any cursor passes them (a full active-set scan
 *   every run covers those). Both tracks are idempotent.
 * - Identity is exact, never LIKE: the title key
 *   bridge:<builder>:<build_id>:<event>:<seq>, matched with `=`, plus a
 *   builder-agnostic body check on exact JSON needles
 *   ("build_id": N, "seq": S) so a hostname change cannot duplicate an
 *   event. Different transitions of one build carry different (event,
 *   seq) pairs; the same event is never delivered twice.
 * - bridge_render(): read-only obligations join over journal bridge
 *   events and live queue state. Buckets stay separate: historical
 *   terminal failures, currently-unresolved work, and outstanding user
 *   tasks (checkpoints). A published NVR is succeeded work even when
 *   an older queue row reads BLOCKED/FAIL (same-NVR only).
 */
#ifndef SBM_BRIDGE_H
#define SBM_BRIDGE_H

#include <sqlite3.h>

#include "core.h"

/* Import missed builder history into the journal. Fail-open: any
 * failure (missing queue.db, locked journal, schema mismatch) yields
 * zero imports and a zero return, never an error to the caller.
 * max_rows bounds one call; the cursor resumes next call. */
int bridge_run(sqlite3 *jdb, long max_rows, long *p_imported,
               long *p_scanned);

/* Requested recipe NVR ("2.14-r4") for a package, or "" when the
 * tree carries no such recipe. Defined in tools.c (shared recipe
 * parser); bridge.c uses it so obligations classifies staleness
 * identically to status. */
void recipe_requested(const char *pkg, char *out, size_t n);

/* Repository newest NVR for a package with its publication stamp, or
 * "" when absent/unreadable. Fail-open: never errors. */
void bridge_repo_newest(const char *pkg, char *nvr, size_t nn,
                        char *pub, size_t pn);

/* Effective package state from requested NVR + actual repository
 * publication + the latest queue attempt - never the latest row
 * alone. state/detail always set; qdb may be NULL (yields UNKNOWN).
 * STALE means the recipe moved past the failed attempt: visible for
 * history, excluded from needs-attention. */
void bridge_effective(sqlite3 *qdb, const char *pkg, const char *req,
                      char *state, size_t sn, char *detail, size_t dn);

/* True when the ISO UTC timestamp is within the last 7 days (recent
 * work prioritised in startup/dashboard; older rows stay listed). */
int bridge_recent(const char *iso);

/* Bounded startup catch-up: open the journal, import, close. Called
 * once per initialize; strictly fail-open and time-bounded so connect
 * never blocks on builder state. */
void bridge_startup_sync(void);

/* Render outstanding builder work into text. Read-only on both
 * databases (journal reads, queue opens read-only). pkg may be NULL
 * for all packages. No task associations are invented: checkpoints
 * link only on exact package match, otherwise rows list unlinked. */
int bridge_render(sqlite3 *jdb, const char *pkg, long limit, gbuf *text);

/* One-line agent-facing summary for initialize instructions.
 * Fail-open: appends nothing when the journal is unavailable. */
void bridge_instruction_line(gbuf *out);

#endif
