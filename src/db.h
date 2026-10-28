#ifndef SBM_DB_H
#define SBM_DB_H

#include <sqlite3.h>
#include <stddef.h>

#include "core.h"

/* Data-source paths: live-system defaults, overridable for hermetic
 * tests. Never NULL. */
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

#endif
