#ifndef SBM_MCP_H
#define SBM_MCP_H

#define SBM_NAME "saphira-build-mcp"
#define SBM_VERSION "0.10.1"

#define SBM_MAX_REQUEST_BYTES (64u * 1024u)
#define SBM_MAX_HEADER_BYTES 16384u
#define SBM_MAX_QUERY_BYTES 8192u
#define SBM_MAX_RESULT_BYTES (256u * 1024u)
#define SBM_MAX_OUTPUT_FILE_BYTES (2u * 1024u * 1024u)

/* tools/list is assembled at runtime (see tools_list_json in
 * tools.c): sixteen read-only tools plus four journal-scoped writes
 * (notes.db only - no recipe, queue, revision, signing, publish,
 * install or restart surface exists by design, not by omission),
 * plus two daemon/operator ops (build_request queues detached work,
 * package_bump applies bumppkg guards; neither signs, publishes nor
 * installs), plus four checkpoint register tools (save/resume/list/
 * close). /etc/saphira/mcp/tools.conf (see conf.h) may additionally
 * expose curated fixed-argv externals and switch off non-checkpoint
 * built-ins per host; the checkpoint register is always exposed. */

int run_stdio(void);

#endif
