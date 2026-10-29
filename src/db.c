#include "db.h"
#include "wincompat.h"

#include <sqlite3.h>
#include <errno.h>
#ifndef _WIN32
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

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
/* Execution provenance (v0.10.0): every journal row can carry where
 * it was written — hostname, username, cwd, stable session id, agent
 * client and the origin of that context ('client' when the caller
 * supplied it, 'server' for server-observed values). All six columns
 * are nullable with no defaults: unknown stays NULL, never
 * fabricated. Pre-v0.10.0 rows read back NULL via the migration
 * below. */
#define PROV_COLS \
    "session_id TEXT,hostname TEXT,username TEXT,cwd TEXT," \
    "agent TEXT,ctx_origin TEXT"

static const char *notes_schema_sql =
    "CREATE TABLE IF NOT EXISTS notes("
    "id INTEGER PRIMARY KEY,package TEXT NOT NULL,kind TEXT NOT NULL,"
    "title TEXT NOT NULL DEFAULT '',body TEXT NOT NULL,"
    "author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',"
    "recipe_commit TEXT,created_at TEXT NOT NULL," PROV_COLS ");"
    "CREATE TABLE IF NOT EXISTS policy_records("
    "id INTEGER PRIMARY KEY,package TEXT NOT NULL,check_name TEXT NOT NULL,"
    "result TEXT NOT NULL,recipe_commit TEXT,recipe_digest TEXT,"
    "registry_digest TEXT,artifact_nvr TEXT,evidence_json TEXT,"
    "author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',"
    "created_at TEXT NOT NULL,supersedes_id INTEGER," PROV_COLS ");"
    "CREATE INDEX IF NOT EXISTS idx_notes_pkg ON notes(package,id);"
    "CREATE INDEX IF NOT EXISTS idx_policy_pkg ON policy_records(package,check_name,id);"
    "CREATE TABLE IF NOT EXISTS checkpoints("
    "id INTEGER PRIMARY KEY,label TEXT NOT NULL,"
    "package TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',"
    "body TEXT NOT NULL,author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',"
    "src_commit TEXT,recipe_commit TEXT,created_at TEXT NOT NULL,"
    "status TEXT NOT NULL DEFAULT 'active',"
    "project TEXT NOT NULL DEFAULT '',workstream TEXT NOT NULL DEFAULT '',"
    "closed_at TEXT,closed_by TEXT," PROV_COLS ");"
    "CREATE INDEX IF NOT EXISTS idx_checkpoints_label ON checkpoints(label,id);"
    "CREATE TABLE IF NOT EXISTS tool_runs("
    "id INTEGER PRIMARY KEY,tool TEXT NOT NULL,argv TEXT NOT NULL,"
    "class TEXT NOT NULL,author TEXT NOT NULL,"
    "recorder TEXT NOT NULL DEFAULT '',exit_status INTEGER NOT NULL,"
    "elapsed_ms INTEGER NOT NULL,truncated INTEGER NOT NULL DEFAULT 0,"
    "timed_out INTEGER NOT NULL DEFAULT 0,created_at TEXT NOT NULL,"
    PROV_COLS ");"
    "CREATE INDEX IF NOT EXISTS idx_tool_runs_tool ON tool_runs(tool,id);"
    /* Phase-2 seam: the session-event journal exists from the start
     * (empty until a writer lands) so transcript capture later plugs
     * into the same retrieval index with no redesign. */
    "CREATE TABLE IF NOT EXISTS session_events("
    "id INTEGER PRIMARY KEY,event TEXT NOT NULL,"
    "package TEXT NOT NULL DEFAULT '',label TEXT NOT NULL DEFAULT '',"
    "title TEXT NOT NULL DEFAULT '',body TEXT NOT NULL DEFAULT '',"
    "author TEXT NOT NULL DEFAULT '',recorder TEXT NOT NULL DEFAULT '',"
    "created_at TEXT NOT NULL," PROV_COLS ");";

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

/* Provenance v3 (0.10.0): the six nullable columns on the four
 * append-only tables. Pre-0.10.0 rows read back NULL (SQLite fills
 * ADD COLUMN without default as NULL) — unknown stays unknown. */
static const char *prov_tables[] = {
	"notes", "policy_records", "checkpoints", "tool_runs", NULL
};
static const char *prov_add[] = {
	"ALTER TABLE %s ADD COLUMN session_id TEXT",
	"ALTER TABLE %s ADD COLUMN hostname TEXT",
	"ALTER TABLE %s ADD COLUMN username TEXT",
	"ALTER TABLE %s ADD COLUMN cwd TEXT",
	"ALTER TABLE %s ADD COLUMN agent TEXT",
	"ALTER TABLE %s ADD COLUMN ctx_origin TEXT",
	NULL
};
static const char *prov_names[] = {
	"session_id", "hostname", "username", "cwd", "agent",
	"ctx_origin", NULL
};

static void ensure_provenance_v3(sqlite3 *db) {
	int t, i;
	for (t = 0; prov_tables[t]; t++) {
		for (i = 0; prov_names[i]; i++) {
			sqlite3_stmt *st = NULL;
			char prag[128], sql[256];
			int found = 0;
			/* Table names are compile-time constants. */
			snprintf(prag, sizeof prag, "PRAGMA table_info(%s)",
			         prov_tables[t]);
			if (sqlite3_prepare_v2(db, prag, -1, &st,
			    NULL) != SQLITE_OK)
				return;
			while (sqlite3_step(st) == SQLITE_ROW) {
				const unsigned char *n =
				    sqlite3_column_text(st, 1);
				if (n && strcmp((const char *)n,
				    prov_names[i]) == 0) {
					found = 1;
					break;
				}
			}
			sqlite3_finalize(st);
			if (!found) {
				snprintf(sql, sizeof sql, prov_add[i],
				         prov_tables[t]);
				sqlite3_exec(db, sql, NULL, NULL, NULL);
			}
		}
	}
}

/* Full-text retrieval index for total_recall. One FTS5 row per
 * journal row, fed by triggers; metadata columns are UNINDEXED
 * (stored for filtering/display, never ranked). fts_ready reports
 * whether the index exists: without FTS5 support the journal stays
 * fully functional and query mode falls back to LIKE. */
static int fts_ready = 0;

int db_fts_ok(void) {
	return fts_ready;
}

static const char *fts_schema_sql =
    "CREATE VIRTUAL TABLE IF NOT EXISTS recall_index USING fts5("
    "session_id UNINDEXED,package UNINDEXED,label UNINDEXED,"
    "kind UNINDEXED,title,body,created_at UNINDEXED,"
    "source_table UNINDEXED,source_id UNINDEXED,tokenize='porter');"
    "CREATE TRIGGER IF NOT EXISTS recall_notes_ai AFTER INSERT ON notes BEGIN "
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id) VALUES("
    "NEW.session_id,NEW.package,'',NEW.kind,NEW.title,NEW.body,"
    "NEW.created_at,'notes',NEW.id); END;"
    "CREATE TRIGGER IF NOT EXISTS recall_checkpoints_ai AFTER INSERT ON checkpoints BEGIN "
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id) VALUES("
    "NEW.session_id,NEW.package,NEW.label,'checkpoint',NEW.title,NEW.body,"
    "NEW.created_at,'checkpoints',NEW.id); END;"
    "CREATE TRIGGER IF NOT EXISTS recall_policy_ai AFTER INSERT ON policy_records BEGIN "
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id) VALUES("
    "NEW.session_id,NEW.package,'','policy',NEW.check_name,NEW.evidence_json,"
    "NEW.created_at,'policy_records',NEW.id); END;"
    "CREATE TRIGGER IF NOT EXISTS recall_toolruns_ai AFTER INSERT ON tool_runs BEGIN "
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id) VALUES("
    "NEW.session_id,'','','tool-run',NEW.tool,NEW.argv,"
    "NEW.created_at,'tool_runs',NEW.id); END;"
    "CREATE TRIGGER IF NOT EXISTS recall_events_ai AFTER INSERT ON session_events BEGIN "
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id) VALUES("
    "NEW.session_id,NEW.package,NEW.label,NEW.event,NEW.title,NEW.body,"
    "NEW.created_at,'session_events',NEW.id); END;";

static const char *fts_backfill_sql =
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id)"
    " SELECT session_id,package,'',kind,title,body,created_at,'notes',id"
    " FROM notes WHERE id NOT IN"
    " (SELECT source_id FROM recall_index WHERE source_table='notes');"
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id)"
    " SELECT session_id,package,label,'checkpoint',title,body,created_at,"
    "'checkpoints',id FROM checkpoints WHERE id NOT IN"
    " (SELECT source_id FROM recall_index WHERE source_table='checkpoints');"
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id)"
    " SELECT session_id,package,'','policy',check_name,evidence_json,"
    "created_at,'policy_records',id FROM policy_records WHERE id NOT IN"
    " (SELECT source_id FROM recall_index WHERE source_table='policy_records');"
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id)"
    " SELECT session_id,'','','tool-run',tool,argv,created_at,'tool_runs',id"
    " FROM tool_runs WHERE id NOT IN"
    " (SELECT source_id FROM recall_index WHERE source_table='tool_runs');"
    "INSERT INTO recall_index(session_id,package,label,kind,title,body,"
    "created_at,source_table,source_id)"
    " SELECT session_id,package,label,event,title,body,created_at,"
    "'session_events',id FROM session_events WHERE id NOT IN"
    " (SELECT source_id FROM recall_index WHERE source_table='session_events');";

static void ensure_fts(sqlite3 *db) {
	char *msg = NULL;
	fts_ready = 0;
	if (sqlite3_exec(db, fts_schema_sql, NULL, NULL, &msg) != SQLITE_OK) {
		sqlite3_free(msg);
		return;
	}
	if (sqlite3_exec(db, fts_backfill_sql, NULL, NULL, &msg) != SQLITE_OK) {
		sqlite3_free(msg);
		return;
	}
	fts_ready = 1;
}

/* The journal self-initialises on a fresh path: create the parent
 * directory when missing so first start works. Only missing
 * components are created (default modes under the process umask);
 * existing directories are never touched, and the rule applies in
 * every mode — it is about the configured path, never the mode. */
static int ensure_parent_dir(const char *path, char *err, size_t errsz) {
	char dir[4096];
	size_t i, n;
	n = strlen(path);
	if (n == 0 || n >= sizeof dir) {
		snprintf(err, errsz, "journal path too long");
		return -1;
	}
	memcpy(dir, path, n + 1);
	for (i = 1; i < n; i++) {
		if (dir[i] == '/') {
			dir[i] = 0;
			if (SBM_MKDIR(dir, 0755) != 0 && errno != EEXIST) {
				snprintf(err, errsz, "cannot create %s: %s",
				         dir, strerror(errno));
				return -1;
			}
			dir[i] = '/';
		}
	}
	return 0;
}

sqlite3 *db_open_notes(char *err, size_t errsz) {
	sqlite3 *db = NULL;
	const char *path = path_notes_db();
	char *msg = NULL;
	int rc;
	if (ensure_parent_dir(path, err, errsz) != 0)
		return NULL;
	rc = sqlite3_open_v2(path, &db,
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
	ensure_provenance_v3(db);
	ensure_fts(db);
	return db;
}

void prov_server_fill(prov_t *p) {
	char cwd[1024];
#ifndef _WIN32
	char host[256];
#endif
	const char *e;
	memset(p, 0, sizeof *p);
	snprintf(p->origin, sizeof p->origin, "server");
	e = getenv("SBM_HOSTNAME");
	if (!e || !e[0]) {
#ifdef _WIN32
		/* COMPUTERNAME is always present on Windows and Wine;
		 * gethostname() would need Winsock startup for zero
		 * benefit here. Absent means unknown (nullable). */
		e = getenv("COMPUTERNAME");
#else
		if (gethostname(host, sizeof host) == 0)
			e = host;
		else
			e = NULL;
#endif
	}
	if (e && e[0])
		snprintf(p->host, sizeof p->host, "%s", e);
	e = getenv("SBM_USERNAME");
	if (!e || !e[0]) {
#ifdef _WIN32
		/* USERNAME is set for every logon session and Wine
		 * prefix; no advapi32 dependency needed. */
		e = getenv("USERNAME");
#else
		struct passwd *pw = getpwuid(geteuid());
		e = (pw && pw->pw_name) ? pw->pw_name : NULL;
#endif
	}
	if (e && e[0])
		snprintf(p->user, sizeof p->user, "%s", e);
	e = getenv("SBM_CWD");
	if ((!e || !e[0]) && SBM_GETCWD(cwd, sizeof cwd))
		e = cwd;
	if (e && e[0])
		snprintf(p->cwd, sizeof p->cwd, "%s", e);
}

void prov_bind(sqlite3_stmt *st, int *idx, const prov_t *p) {
	int i = *idx;
	if (p->session[0])
		sqlite3_bind_text(st, i, p->session, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, i);
	i++;
	if (p->host[0])
		sqlite3_bind_text(st, i, p->host, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, i);
	i++;
	if (p->user[0])
		sqlite3_bind_text(st, i, p->user, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, i);
	i++;
	if (p->cwd[0])
		sqlite3_bind_text(st, i, p->cwd, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, i);
	i++;
	if (p->agent[0])
		sqlite3_bind_text(st, i, p->agent, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, i);
	i++;
	sqlite3_bind_text(st, i, p->origin, -1, SQLITE_TRANSIENT);
	i++;
	*idx = i;
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
