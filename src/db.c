#include "db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *env_or(const char *name, const char *dflt) {
	const char *v = getenv(name);
	return (v && v[0]) ? v : dflt;
}

const char *path_recipes(void) { return env_or("SBM_RECIPES", "/recipes"); }
const char *path_queue_db(void) { return env_or("SBM_QUEUE_DB", "/var/lib/saphira-build/queue.db"); }
const char *path_repo_dir(void) { return env_or("SBM_REPO_DIR", "/out/stage4/packages/hatched/x86_64"); }
const char *path_incoming(void) { return env_or("SBM_INCOMING", "/out/stage4/incoming/x86_64"); }
const char *path_tsv(void) { return env_or("SBM_TSV", "/usr/share/saphira/accounts.tsv"); }
const char *path_passwd(void) { return env_or("SBM_PASSWD", "/etc/passwd"); }
const char *path_group(void) { return env_or("SBM_GROUP", "/etc/group"); }
const char *path_apkdb(void) { return env_or("SBM_APKDB", "/lib/apk/db/installed"); }

sqlite3 *db_open_ro(const char *path, char *err, size_t errsz) {
	sqlite3 *db = NULL;
	int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		if (db) {
			snprintf(err, errsz, "cannot open %s: %s", path, sqlite3_errmsg(db));
			sqlite3_close(db);
		} else {
			snprintf(err, errsz, "cannot open %s: %s", path, sqlite3_errstr(rc));
		}
		return NULL;
	}
	return db;
}

void sb_cell(gbuf *b, sqlite3_stmt *st, int col) {
	if (sqlite3_column_type(st, col) == SQLITE_NULL) {
		gbuf_adds(b, "-");
		return;
	}
	/* sqlite3_column_text converts numbers and copies out below
	 * before the next step, so the transient pointer is safe here. */
	gbuf_adds(b, (const char *)sqlite3_column_text(st, col));
}
