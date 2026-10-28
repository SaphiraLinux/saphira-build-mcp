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
const char *path_notes_db(void) { return env_or("SBM_NOTES_DB", "/var/lib/saphira-build/notes.db"); }

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

/* Journal schema is owned by this code (CREATE TABLE IF NOT EXISTS on
 * every open): notes.db must never be carried in /recipes except blank
 * and empty, and no migration tooling is needed for append-only tables. */
static const char *notes_schema_sql =
    "CREATE TABLE IF NOT EXISTS notes("
    "id INTEGER PRIMARY KEY,package TEXT NOT NULL,kind TEXT NOT NULL,"
    "title TEXT NOT NULL DEFAULT '',body TEXT NOT NULL,"
    "author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',"
    "recipe_commit TEXT,created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS policy_records("
    "id INTEGER PRIMARY KEY,package TEXT NOT NULL,check_name TEXT NOT NULL,"
    "result TEXT NOT NULL,recipe_commit TEXT,recipe_digest TEXT,"
    "registry_digest TEXT,artifact_nvr TEXT,evidence_json TEXT,"
    "author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',"
    "created_at TEXT NOT NULL,supersedes_id INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_notes_pkg ON notes(package,id);"
    "CREATE INDEX IF NOT EXISTS idx_policy_pkg ON policy_records(package,check_name,id);"
    "CREATE TABLE IF NOT EXISTS checkpoints("
    "id INTEGER PRIMARY KEY,label TEXT NOT NULL,"
    "package TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',"
    "body TEXT NOT NULL,author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',"
    "src_commit TEXT,recipe_commit TEXT,created_at TEXT NOT NULL,"
    "status TEXT NOT NULL DEFAULT 'active',"
    "project TEXT NOT NULL DEFAULT '',workstream TEXT NOT NULL DEFAULT '',"
    "closed_at TEXT,closed_by TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_checkpoints_label ON checkpoints(label,id);"
    "CREATE TABLE IF NOT EXISTS tool_runs("
    "id INTEGER PRIMARY KEY,tool TEXT NOT NULL,argv TEXT NOT NULL,"
    "class TEXT NOT NULL,author TEXT NOT NULL,"
    "recorder TEXT NOT NULL DEFAULT '',exit_status INTEGER NOT NULL,"
    "elapsed_ms INTEGER NOT NULL,truncated INTEGER NOT NULL DEFAULT 0,"
    "timed_out INTEGER NOT NULL DEFAULT 0,created_at TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS idx_tool_runs_tool ON tool_runs(tool,id);";

/* Checkpoints v2 (0.7.0): the active-by-default register. New columns
 * on the append-only checkpoints table; pre-0.7.0 rows read back as
 * active with empty project/workstream (SQLite fills ADD COLUMN ..
 * DEFAULT into existing rows; closed_at/closed_by stay NULL). Status
 * values are code-enforced ('active'/'closed', see tools.c); close
 * never deletes, it only flips status. */
static const char *checkpoint_v2_add[] = {
	"ALTER TABLE checkpoints ADD COLUMN status TEXT NOT NULL DEFAULT 'active'",
	"ALTER TABLE checkpoints ADD COLUMN project TEXT NOT NULL DEFAULT ''",
	"ALTER TABLE checkpoints ADD COLUMN workstream TEXT NOT NULL DEFAULT ''",
	"ALTER TABLE checkpoints ADD COLUMN closed_at TEXT",
	"ALTER TABLE checkpoints ADD COLUMN closed_by TEXT",
	NULL
};
static const char *checkpoint_v2_names[] = {
	"status", "project", "workstream", "closed_at", "closed_by", NULL
};

static void ensure_checkpoints_v2(sqlite3 *db) {
	int i;
	sqlite3_stmt *st = NULL;
	/* One PRAGMA scan per missing-candidate column: the table is
	 * tiny and this runs once per journal open. */
	for (i = 0; checkpoint_v2_names[i]; i++) {
		int found = 0;
		if (sqlite3_prepare_v2(db, "PRAGMA table_info(checkpoints)",
		    -1, &st, NULL) != SQLITE_OK)
			return;
		while (sqlite3_step(st) == SQLITE_ROW) {
			const unsigned char *n = sqlite3_column_text(st, 1);
			if (n && strcmp((const char *)n,
			    checkpoint_v2_names[i]) == 0) {
				found = 1;
				break;
			}
		}
		sqlite3_finalize(st);
		st = NULL;
		if (!found)
			sqlite3_exec(db, checkpoint_v2_add[i], NULL, NULL, NULL);
	}
}

sqlite3 *db_open_notes(char *err, size_t errsz) {
	sqlite3 *db = NULL;
	const char *path = path_notes_db();
	char *msg = NULL;
	int rc = sqlite3_open_v2(path, &db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK) {
		if (db) {
			snprintf(err, errsz, "cannot open %s: %s", path, sqlite3_errmsg(db));
			sqlite3_close(db);
		} else {
			snprintf(err, errsz, "cannot open %s: %s", path, sqlite3_errstr(rc));
		}
		return NULL;
	}
	rc = sqlite3_exec(db, notes_schema_sql, NULL, NULL, &msg);
	if (rc != SQLITE_OK) {
		snprintf(err, errsz, "cannot init %s: %s", path, msg ? msg : "schema error");
		sqlite3_free(msg);
		sqlite3_close(db);
		return NULL;
	}
	ensure_checkpoints_v2(db);
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
