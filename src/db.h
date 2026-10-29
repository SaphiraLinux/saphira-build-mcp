#ifndef SBM_DB_H
#define SBM_DB_H

#include <sqlite3.h>
#include <stddef.h>

#include "core.h"

/* Data-source paths: live-system defaults, overridable for hermetic
 * tests. Never NULL. */
const char *env_or(const char *name, const char *dflt);
const char *env_or(const char *name, const char *dflt);
const char *path_recipes(void);
const char *path_queue_db(void);
const char *path_repo_dir(void);
const char *path_incoming(void);
const char *path_tsv(void);
const char *path_passwd(void);
const char *path_group(void);
const char *path_apkdb(void);
const char *path_notes_db(void);

/* Open read-only. Returns NULL with err set on failure. */
sqlite3 *db_open_ro(const char *path, char *err, size_t errsz);

/* Open the journal read-write, creating it and its tables when absent.
 * The journal is the MCP's own database (Egg-local); Keeper/Homer are
 * only backup destinations fed by SQLite-safe snapshots
 * (`sqlite3 DB .backup SNAPSHOT` via the backup API), never by
 * copying the live file. Returns NULL with err set on failure. */
sqlite3 *db_open_notes(char *err, size_t errsz);

/* Append one column as text; SQL NULL renders "-". */
void sb_cell(gbuf *b, sqlite3_stmt *st, int col);

/* Execution provenance carried on journal rows. All fields nullable
 * in storage; empty here means unknown (bind NULL, never fabricate).
 * origin is "client" when the caller supplied context, "server" for
 * server-observed values. */
typedef struct {
	char session[128];
	char host[256];
	char user[256];
	char cwd[1024];
	char agent[64];
	char origin[16];
} prov_t;

/* Fill from SBM_* fixture overrides, else live server values
 * (gethostname/getpwuid/getcwd); failures leave fields empty.
 * Origin is always "server": the client overlay in tools.c
 * upgrades it to "client" when the caller supplies context. */
void prov_server_fill(prov_t *p);

/* Bind the six provenance columns at *idx (advanced past them);
 * empty fields bind NULL, never fabricated. */
void prov_bind(sqlite3_stmt *st, int *idx, const prov_t *p);

/* Full-text retrieval index availability (1 when recall_index
 * exists, else query mode falls back to LIKE). Evaluated on every
 * journal open; process-lifetime thereafter. */
int db_fts_ok(void);

#endif
