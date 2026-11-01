#ifndef SBM_RUNNER_H
#define SBM_RUNNER_H

#include "conf.h"
#include "core.h"

/* Execute a registered external tool. Fixed conf argv only: no
 * shell anywhere on this path. read/test tools take no caller
 * input and run with stdin from /dev/null; ask tools additionally
 * accept one caller string (prompt/prompt_len), delivered on a
 * dedicated stdin pipe that is closed with EOF after writing.
 * Prompt content is never stored: only its byte length is noted in
 * the audit row. Captures combined stdout+stderr up to max_bytes,
 * enforces timeout_s, optionally wraps class-test runs in bwrap.
 * Every attempt is audited best-effort into the journal tool_runs
 * table. Returns 0 with text/is_error set (is_error=1 for timeout,
 * sandbox refusal, spawn failure or nonzero exit); returns -1 only
 * when the tool must not run at all (unknown here - dispatch
 * handles that). */
int runner_exec(const ext_tool *t, const char *author,
                const char *recorder, const char *prompt, size_t prompt_len,
                gbuf *out, int *is_error);

#endif
