#ifndef SBM_RUNNER_H
#define SBM_RUNNER_H

#include "conf.h"
#include "core.h"

/* Execute a registered external tool. Fixed conf argv only: caller
 * arguments are never consulted, no shell exists anywhere on this
 * path. Captures combined stdout+stderr up to max_bytes, enforces
 * timeout_s, optionally wraps class-test runs in bwrap. Every attempt
 * is audited best-effort into the journal tool_runs table. Returns 0
 * with text/is_error set (is_error=1 for timeout, sandbox refusal,
 * spawn failure or nonzero exit); returns -1 only when the tool must
 * not run at all (unknown here - dispatch handles that). */
int runner_exec(const ext_tool *t, const char *author,
                const char *recorder, gbuf *out, int *is_error);

#endif
