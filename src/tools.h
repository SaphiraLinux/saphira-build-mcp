#ifndef SBM_TOOLS_H
#define SBM_TOOLS_H

#include "core.h"
#include "json.h"

/* Dispatch one tools/call by name. Returns 0 and fills text/is_error
 * (is_error=1 means a valid call that failed its lookup, reported as
 * MCP isError, not a transport error). Returns -1 for unknown tool
 * names (caller maps to -32602). text is always NUL-terminated. */
int tools_call(const char *name, jv *args, gbuf *text, int *is_error);

/* Recorder identity for journal attribution: MCP clientInfo
 * name/version, captured per connection. Client-reported and
 * unverified - attribution, never authentication. */
void sbm_set_recorder(const char *recorder);

/* Build the initialize instructions: static checkpoint-register
 * guidance plus the live active set (first five plus total count).
 * Read-only against the journal; fails open to static guidance when
 * the journal is unavailable or legacy. */
void checkpoint_instructions(gbuf *out);

#endif
