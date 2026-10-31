/* Builder/journal bridge: reconcile missed history, render obligations.
 *
 * See bridge.h for the separation contract. Notes on the tricky parts:
 *
 * Two-track catch-up. Terminal FAIL/INTERRUPTED rows never change, so a
 * monotonic id cursor over them is reliable and resumable. Everything
 * else (QUEUED/BLOCKED/BUILDING/PASS-unpublished) can change state after
 * the cursor passes: those are re-scanned whole every run (the active
 * set is small by construction). A failure is missed only if both
 * tracks skip it, which neither does: the cursor pages the full id
 * range, the active set ignores the cursor.
 *
 * Identity. Title key bridge:<builder>:<id>:<event>:<seq>, compared
 * with `=`. The builder and this reconciler compute it identically
 * (builder_id: SBM_BUILDER_ID, else SBM_BUILDER_NAME, else hostname;
 * row rows prefer their recorded builder_host). A hostname change
 * across restarts would fork the key, so existence is ALSO checked
 * builder-agnostically: exact JSON needles `"build_id": N` and
 * `"seq": S` inside the body (the builder writes sort_keys JSON with
 * `": "` separators; that format is part of the contract, asserted by
 * the builder's own tests). No LIKE anywhere on this path.
 *
 * Concurrency. The builder writes with timeout=0 INSERT OR IGNORE; this
 * reconciler does the same. A partial UNIQUE index on bridge titles
 * makes same-key doubles impossible; without it (old journal) doubles
 * are still harmless because rendering groups by title.
 */

#include "bridge.h"

#include "db.h"
#include "wincompat.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BRIDGE_VERSION 1
#define BRIDGE_CURSOR_TITLE "bridge cursor"
#define STALLED_AFTER_S 3600L
#define AWAITING_AFTER_S 86400L
#define ACTIVE_CAP 2000
#define HIST_PAGE 200

static void now_iso(char *out, size_t n) {
	time_t t = time(NULL);
	struct tm tm;
	sb_gmtime(&t, &tm);
	snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
	         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	         tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Builder name for rows that record none: same precedence as the
 * builder's own builder_id() (SBM_BUILDER_ID, else SBM_BUILDER_NAME,
 * else hostname), so reconcile keys match builder keys. Kept inline
 * (not shared) because the journal writer and the queue reader must
 * agree without a new cross-module contract. */
static void reconcile_builder(const char *recorded, char *out, size_t n) {
	const char *e;
	if (recorded && recorded[0]) {
		snprintf(out, n, "%s", recorded);
		return;
	}
	e = getenv("SBM_BUILDER_ID");
	if (!e || !e[0])
		e = getenv("SBM_BUILDER_NAME");
	if (e && e[0]) {
		snprintf(out, n, "%s", e);
		return;
	}
#ifdef _WIN32
	e = getenv("COMPUTERNAME");
	if (e && e[0]) {
		snprintf(out, n, "%s", e);
		return;
	}
#else
	{
		char host[256];
		if (gethostname(host, sizeof host) == 0 && host[0]) {
			snprintf(out, n, "%s", host);
			return;
		}
	}
#endif
	snprintf(out, n, "unknown-builder");
}

static long parse_iso_age_s(const char *iso) {
	struct tm tm;
	time_t then, now;
	int y, mo, d, h, mi, s;
	if (!iso || !iso[0])
		return -1;
	if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2dZ",
	           &y, &mo, &d, &h, &mi, &s) != 6)
		return -1;
	memset(&tm, 0, sizeof tm);
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = s;
	then = sb_timegm(&tm);
	now = time(NULL);
	if (then == (time_t)-1)
		return -1;
	return (long)(now - then);
}

/* Exact-needle blocked_seq from result_json. The builder stores
 * {"bridge": {"blocked_seq": N}, ...}; absent means 1. */
static int blocked_seq_of(const char *result_json) {
	const char *p;
	int seq = 0;
	if (!result_json)
		return 1;
	p = strstr(result_json, "\"blocked_seq\":");
	if (!p)
		return 1;
	if (sscanf(p + 15, " %d", &seq) != 1 || seq < 1)
		return 1;
	return seq;
}

static int has_blocked_seq(const char *result_json) {
	return result_json &&
	       strstr(result_json, "\"blocked_seq\":") != NULL;
}

static void bridge_key(char *out, size_t n, const char *builder,
                       long long build_id, const char *event, int seq) {
	snprintf(out, n, "bridge:%s:%lld:%s:%d", builder, build_id,
	         event, seq);
}

static void ensure_bridge_index(sqlite3 *jdb) {
	/* Partial unique index: same-key doubles become impossible even
	 * when the opportunistic builder writer and this reconciler race.
	 * Fail-open: old SQLite builds without partial-index support keep
	 * working; rendering dedupes by title. The cursor lives in its
	 * own bridge_state table so bridge bookkeeping never disturbs
	 * notes/session_events id sequences asserted by tests. */
	sqlite3_exec(jdb,
	             "CREATE UNIQUE INDEX IF NOT EXISTS"
	             " idx_session_events_bridge ON session_events(title)"
	             " WHERE title LIKE 'bridge:%'",
	             NULL, NULL, NULL);
	sqlite3_exec(jdb,
	             "CREATE TABLE IF NOT EXISTS bridge_state("
	             "key TEXT PRIMARY KEY,value TEXT NOT NULL)",
	             NULL, NULL, NULL);
}

static long bridge_cursor_get(sqlite3 *jdb) {
	sqlite3_stmt *st = NULL;
	long cur = 0;
	if (sqlite3_prepare_v2(jdb,
	    "SELECT value FROM bridge_state WHERE key='cursor'",
	    -1, &st, NULL) != SQLITE_OK)
		return 0;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *b = sqlite3_column_text(st, 0);
		if (b)
			cur = atol((const char *)b);
	}
	sqlite3_finalize(st);
	return cur < 0 ? 0 : cur;
}

static void bridge_cursor_set(sqlite3 *jdb, long id) {
	sqlite3_stmt *st = NULL;
	char body[32];
	snprintf(body, sizeof body, "%ld", id);
	if (sqlite3_prepare_v2(jdb,
	    "INSERT INTO bridge_state(key,value) VALUES('cursor',?1)"
	    " ON CONFLICT(key) DO UPDATE SET value=excluded.value",
	    -1, &st, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(st, 1, body, -1, SQLITE_TRANSIENT);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

/* Builder-agnostic existence: same event with body needles
 * "build_id": N and "seq": S. Exact strstr, never LIKE. */
static int bridge_event_present(sqlite3 *jdb, const char *package,
                                const char *event, const char *key,
                                long long build_id, int seq) {
	sqlite3_stmt *st = NULL;
	char id_needle[64], seq_needle[32];
	int found = 0;
	snprintf(id_needle, sizeof id_needle, "\"build_id\": %lld", build_id);
	snprintf(seq_needle, sizeof seq_needle, "\"seq\": %d", seq);
	if (sqlite3_prepare_v2(jdb,
	    "SELECT title,body FROM session_events"
	    " WHERE event=?1 AND package=?2", -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, event, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, package, -1, SQLITE_TRANSIENT);
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *t = sqlite3_column_text(st, 0);
		const unsigned char *b = sqlite3_column_text(st, 1);
		if (t && strcmp((const char *)t, key) == 0) {
			found = 1;
			break;
		}
		if (b && strstr((const char *)b, id_needle) &&
		    strstr((const char *)b, seq_needle)) {
			found = 1;
			break;
		}
	}
	sqlite3_finalize(st);
	return found;
}

struct expected {
	char event[48];
	int seq;
};

struct rowshape {
	long long id;
	char package[128];
	char version[64];
	char revision[16];
	char state[16];
	char builder[256];
	char queued_at[32];
	char started_at[32];
	char finished_at[32];
	char published_at[32];
};

/* Compute the expected bridge events for one queue row, mirroring the
 * builder's bridge_emit call sites exactly (FAIL/INTERRUPTED seq 1,
 * BLOCKED blocked_seq, QUEUED-with-blocked-history emits the recorded
 * blocked event plus unblocked, stalled/awaiting buckets). Returns the
 * count filled into ev (capacity 4). */
static int expected_for_row(const struct rowshape *r, const char *result_json,
                            struct expected *ev) {
	int n = 0;
	long age;
	if (strcmp(r->state, "FAIL") == 0) {
		snprintf(ev[n].event, sizeof ev[n].event, "build.failed");
		ev[n].seq = 1;
		return 1;
	}
	if (strcmp(r->state, "INTERRUPTED") == 0) {
		snprintf(ev[n].event, sizeof ev[n].event, "build.interrupted");
		ev[n].seq = 1;
		return 1;
	}
	if (strcmp(r->state, "BLOCKED") == 0) {
		snprintf(ev[n].event, sizeof ev[n].event, "build.blocked");
		ev[n].seq = blocked_seq_of(result_json);
		return 1;
	}
	if (strcmp(r->state, "QUEUED") == 0 && has_blocked_seq(result_json)) {
		int s = blocked_seq_of(result_json);
		snprintf(ev[n].event, sizeof ev[n].event, "build.blocked");
		ev[n].seq = s;
		n++;
		snprintf(ev[n].event, sizeof ev[n].event, "build.unblocked");
		ev[n].seq = s;
		n++;
	}
	if (strcmp(r->state, "QUEUED") == 0 ||
	    strcmp(r->state, "BUILDING") == 0) {
		age = parse_iso_age_s(r->started_at[0] ? r->started_at : r->queued_at);
		if (age >= STALLED_AFTER_S && n < 4) {
			const char *bucket = "1h";
			if (age >= 86400L)
				bucket = "24h";
			else if (age >= 21600L)
				bucket = "6h";
			snprintf(ev[n].event, sizeof ev[n].event,
			         "build.stalled.%s", bucket);
			ev[n].seq = 1;
			n++;
		}
		return n;
	}
	if (strcmp(r->state, "PASS") == 0 && !r->published_at[0]) {
		age = parse_iso_age_s(r->finished_at);
		if (age >= AWAITING_AFTER_S && n < 4) {
			snprintf(ev[n].event, sizeof ev[n].event,
			         "build.awaiting_signing");
			ev[n].seq = 1;
			n++;
		}
	}
	return n;
}

static const char *col_text(sqlite3_stmt *st, int col, char *buf, size_t n) {
	const unsigned char *v = sqlite3_column_text(st, col);
	if (!v || !v[0]) {
		if (n)
			buf[0] = 0;
		return buf;
	}
	snprintf(buf, n, "%s", (const char *)v);
	return buf;
}

static int import_one(sqlite3 *jdb, const struct rowshape *r,
                      const char *result_json, const char *event, int seq,
                      const char *nvr, const char *detail) {
	char key[512], body[2048], created[32], label[160];
	sqlite3_stmt *st = NULL;
	prov_t pv;
	char builder[256];
	int rc;
	reconcile_builder(r->builder, builder, sizeof builder);
	bridge_key(key, sizeof key, builder, r->id, event, seq);
	if (bridge_event_present(jdb, r->package, event, key, r->id, seq))
		return 0;
	snprintf(label, sizeof label, "%s", nvr);
	snprintf(body, sizeof body,
	         "{\"bridge_v\": %d, \"build_id\": %lld,"
	         " \"builder\": \"%s\", \"detail\": \"%s\","
	         " \"event\": \"%s\", \"nvr\": \"%s\","
	         " \"package\": \"%s\", \"seq\": %d,"
	         " \"state\": \"%s\"}",
	         BRIDGE_VERSION, r->id, builder, detail ? detail : "",
	         event, nvr, r->package, seq, r->state);
	now_iso(created, sizeof created);
	prov_server_fill(&pv);
	if (sqlite3_prepare_v2(jdb,
	    "INSERT OR IGNORE INTO session_events(event,package,label,title,"
	    "body,author,recorder,created_at,"
	    "session_id,hostname,username,cwd,agent,ctx_origin)"
	    " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
	    -1, &st, NULL) != SQLITE_OK)
		return 0;
	{
		int i = 1;
		sqlite3_bind_text(st, i++, event, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, i++, r->package, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, i++, label, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, i++, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, i++, body, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, i++, "saphira-build-mcp", -1, SQLITE_STATIC);
		sqlite3_bind_text(st, i++, "mcp-reconcile", -1, SQLITE_STATIC);
		sqlite3_bind_text(st, i++, created, -1, SQLITE_TRANSIENT);
		prov_bind(st, &i, &pv);
		rc = sqlite3_step(st);
	}
	sqlite3_finalize(st);
	(void)result_json;
	return rc == SQLITE_DONE &&
	       sqlite3_changes(jdb) > 0 ? 1 : 0;
}

static void row_nvr(const struct rowshape *r, char *out, size_t n) {
	if (r->version[0] && r->revision[0])
		snprintf(out, n, "%s-r%s", r->version, r->revision);
	else if (r->version[0])
		snprintf(out, n, "%s", r->version);
	else if (n)
		out[0] = 0;
}

static int fill_row(sqlite3_stmt *st, struct rowshape *r, char *result_json,
                    size_t rjsz) {
	const unsigned char *v;
	memset(r, 0, sizeof *r);
	r->id = sqlite3_column_int64(st, 0);
	col_text(st, 1, r->package, sizeof r->package);
	col_text(st, 2, r->version, sizeof r->version);
	col_text(st, 3, r->revision, sizeof r->revision);
	col_text(st, 4, r->state, sizeof r->state);
	v = sqlite3_column_text(st, 5);
	snprintf(result_json, rjsz, "%s", v ? (const char *)v : "{}");
	col_text(st, 6, r->builder, sizeof r->builder);
	col_text(st, 7, r->queued_at, sizeof r->queued_at);
	col_text(st, 8, r->started_at, sizeof r->started_at);
	col_text(st, 9, r->finished_at, sizeof r->finished_at);
	col_text(st, 10, r->published_at, sizeof r->published_at);
	return r->package[0] != 0;
}

static int process_row(sqlite3 *jdb, const struct rowshape *r,
                       const char *result_json) {
	struct expected ev[4];
	char nvr[160];
	int i, n, imported = 0;
	n = expected_for_row(r, result_json, ev);
	if (n <= 0)
		return 0;
	row_nvr(r, nvr, sizeof nvr);
	for (i = 0; i < n; i++)
		imported += import_one(jdb, r, result_json, ev[i].event,
		                       ev[i].seq, nvr, "");
	return imported;
}

int bridge_run(sqlite3 *jdb, long max_rows, long *p_imported,
               long *p_scanned) {	sqlite3 *qdb = NULL;
	sqlite3_stmt *st = NULL;
	char dberr[256], qb[256];
	struct rowshape r;
	char result_json[8192];
	long imported = 0, scanned = 0, budget;
	long cursor, high = 0;
	if (p_imported)
		*p_imported = 0;
	if (p_scanned)
		*p_scanned = 0;
	if (!jdb)
		return 0;
	ensure_bridge_index(jdb);
	snprintf(qb, sizeof qb, "%s", path_queue_db());
	qdb = db_open_ro(qb, dberr, sizeof dberr);
	if (!qdb)
		return 0;
	budget = max_rows > 0 ? max_rows : HIST_PAGE;
	/* Track 1: volatile states, full scan, no cursor. A BLOCKED row
	 * that flips to QUEUED after the cursor passed is still seen
	 * here; terminal history below never replays it. */
	if (sqlite3_prepare_v2(qdb,
	    "SELECT id,package,version,revision,state,result_json,"
	    "builder_host,queued_at,started_at,finished_at,published_at"
	    " FROM builds WHERE finished_at IS NULL"
	    " OR state IN ('FAIL','INTERRUPTED','BLOCKED')"
	    " OR (state='PASS' AND published_at IS NULL)"
	    " ORDER BY id LIMIT 2000", -1, &st, NULL) == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW && scanned < budget + ACTIVE_CAP) {
			if (!fill_row(st, &r, result_json, sizeof result_json))
				continue;
			scanned++;
			imported += process_row(jdb, &r, result_json);
		}
	}
	sqlite3_finalize(st);
	st = NULL;
	/* Track 2: terminal rows by monotonic cursor. FAIL/INTERRUPTED
	 * never change state, so advancing past them loses nothing;
	 * anything volatile is covered by track 1. */
	cursor = bridge_cursor_get(jdb);
	if (sqlite3_prepare_v2(qdb,
	    "SELECT id,package,version,revision,state,result_json,"
	    "builder_host,queued_at,started_at,finished_at,published_at"
	    " FROM builds WHERE id>?1 AND state IN ('FAIL','INTERRUPTED')"
	    " ORDER BY id LIMIT 200", -1, &st, NULL) == SQLITE_OK) {
		long pages = 0;
		sqlite3_bind_int64(st, 1, cursor);
		while (sqlite3_step(st) == SQLITE_ROW && pages < budget) {
			if (!fill_row(st, &r, result_json, sizeof result_json))
				continue;
			scanned++;
			pages++;
			imported += process_row(jdb, &r, result_json);
			if (r.id > high)
				high = r.id;
		}
	}
	sqlite3_finalize(st);
	if (high > cursor)
		bridge_cursor_set(jdb, high);
	sqlite3_close(qdb);
	if (p_imported)
		*p_imported = imported;
	if (p_scanned)
		*p_scanned = scanned;
	return 0;
}

/* obligations rendering: three buckets, no invented links. */

struct obrow {
	char package[128];
	char nvr[160];
	char state[32];
	char detail[512];
	char finished[32];
	int recent;
};

static int published_nvr_here(sqlite3 *qdb, const char *pkg,
                              const char *ver, const char *rev) {
	sqlite3_stmt *st = NULL;
	int hit = 0;
	if (sqlite3_prepare_v2(qdb,
	    "SELECT 1 FROM builds WHERE package=?1 AND version=?2"
	    " AND revision=?3 AND published_at IS NOT NULL LIMIT 1",
	    -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, ver, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, rev, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW)
		hit = 1;
	sqlite3_finalize(st);
	return hit;
}

int bridge_render(sqlite3 *jdb, const char *pkg, long limit, gbuf *text) {
	sqlite3 *qdb = NULL;
	sqlite3_stmt *st = NULL;
	char dberr[256];
	struct obrow failed[256], waiting[256], awaiting[64], stale[256];
	int nf = 0, nw = 0, na = 0, ns = 0;
	int nfr = 0, nwr = 0, nar = 0;
	int i, qok = 0;
	long shown = 0;
	char tmp[1024];
	(void)pkg;
	qdb = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (qdb)
		qok = 1;
	if (qdb && sqlite3_prepare_v2(qdb,
	    "SELECT id,package,version,revision,state,result_json,"
	    "builder_host,queued_at,started_at,finished_at,published_at"
	    " FROM builds WHERE id IN"
	    " (SELECT max(id) FROM builds GROUP BY package)"
	    " ORDER BY package", -1, &st, NULL) == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW && shown < limit + 512) {
			struct rowshape r;
			char result_json[8192], nvr[160];
			const char *state;
			if (!fill_row(st, &r, result_json, sizeof result_json))
				continue;
			if (pkg && pkg[0] && strcmp(r.package, pkg) != 0)
				continue;
			shown++;
			if (strcmp(r.state, "PASS") == 0 && r.published_at[0])
				continue;
			state = r.state;
			row_nvr(&r, nvr, sizeof nvr);
			/* Published-NVR rule, same-NVR only: a BLOCKED/FAIL
			 * latest row whose exact NVR is published is
			 * succeeded work, never outstanding. */
			if ((strcmp(state, "BLOCKED") == 0 ||
			     strcmp(state, "FAIL") == 0) &&
			    published_nvr_here(qdb, r.package, r.version,
			                       r.revision))
				continue;
			if (strcmp(state, "FAIL") == 0 ||
			    strcmp(state, "INTERRUPTED") == 0) {
				/* STALE: the recipe moved past this attempt.
				 * Visible for history, excluded from the
				 * needs-attention failed bucket. */
				char req[160] = "";
				int is_stale = 0;
				if (strcmp(state, "FAIL") == 0) {
					recipe_requested(r.package, req,
					                 sizeof req);
					if (req[0] && nvr[0] &&
					    strcmp(req, nvr) != 0)
						is_stale = 1;
				}
				if (is_stale) {
					if (ns < 256) {
						snprintf(stale[ns].package,
						         sizeof stale[ns].package,
						         "%s", r.package);
						snprintf(stale[ns].nvr,
						         sizeof stale[ns].nvr,
						         "%s", nvr);
						snprintf(stale[ns].state,
						         sizeof stale[ns].state,
						         "STALE");
						snprintf(stale[ns].finished,
						         sizeof stale[ns].finished,
						         "%s", r.finished_at);
						snprintf(stale[ns].detail,
						         sizeof stale[ns].detail,
						         "recipe now %s", req);
						stale[ns].recent = bridge_recent(
						    r.finished_at);
						ns++;
					}
				} else if (nf < 256) {
					snprintf(failed[nf].package,
					         sizeof failed[nf].package, "%s",
					         r.package);
					snprintf(failed[nf].nvr,
					         sizeof failed[nf].nvr, "%s", nvr);
					snprintf(failed[nf].state,
					         sizeof failed[nf].state, "%s",
					         state);
					snprintf(failed[nf].finished,
					         sizeof failed[nf].finished, "%s",
					         r.finished_at);
					if (bridge_recent(r.finished_at)) {
						failed[nf].recent = 1;
						nfr++;
					}
					nf++;
				}
			} else if (strcmp(state, "BLOCKED") == 0 ||
			           (strcmp(state, "QUEUED") == 0 ||
			            strcmp(state, "BUILDING") == 0)) {
				long age = parse_iso_age_s(r.started_at[0]
				                           ? r.started_at
				                           : r.queued_at);
				int stale_run = (strcmp(state, "QUEUED") == 0 ||
				                 strcmp(state, "BUILDING") == 0) &&
				                age >= STALLED_AFTER_S;
				if (strcmp(state, "BLOCKED") == 0 || stale_run) {
					if (nw < 256) {
						snprintf(waiting[nw].package,
						         sizeof waiting[nw].package,
						         "%s", r.package);
						snprintf(waiting[nw].nvr,
						         sizeof waiting[nw].nvr,
						         "%s", nvr);
						snprintf(waiting[nw].state,
						         sizeof waiting[nw].state,
						         "%s",
						         strcmp(state, "BLOCKED") == 0
						             ? "BLOCKED"
						             : "STALLED");
						snprintf(waiting[nw].finished,
						         sizeof waiting[nw].finished,
						         "%s", r.started_at[0]
						                  ? r.started_at
						                  : r.queued_at);
						if (bridge_recent(r.started_at[0]
						                  ? r.started_at
						                  : r.queued_at)) {
							waiting[nw].recent = 1;
							nwr++;
						}
						nw++;
					}
				}
			} else if (strcmp(state, "PASS") == 0) {
				long age = parse_iso_age_s(r.finished_at);
				if (age >= AWAITING_AFTER_S && na < 64) {
					snprintf(awaiting[na].package,
					         sizeof awaiting[na].package,
					         "%s", r.package);
					snprintf(awaiting[na].nvr,
					         sizeof awaiting[na].nvr, "%s", nvr);
					snprintf(awaiting[na].state,
					         sizeof awaiting[na].state, "%s",
					         "AWAITING SIGNING");
						snprintf(awaiting[na].finished,
						         sizeof awaiting[na].finished,
						         "%s", r.finished_at);
						if (bridge_recent(r.finished_at)) {
							awaiting[na].recent = 1;
							nar++;
						}
						na++;
				}
			}
		}
	}
	sqlite3_finalize(st);
	snprintf(tmp, sizeof tmp,
	         "outstanding builder work (needs attention first;"
	         " stale recipe attempts listed for reference):\n");
	gbuf_adds(text, tmp);
	snprintf(tmp, sizeof tmp,
	         "failed (ran and failed, no later success;"
	         " %d recent of %d):\n", nfr, nf);
	gbuf_adds(text, tmp);
	if (!nf)
		gbuf_adds(text, "  (none)\n");
	for (i = 0; i < nf && i < limit; i++) {
		snprintf(tmp, sizeof tmp, "  %s %s %s since %s\n",
		         failed[i].package, failed[i].nvr, failed[i].state,
		         failed[i].finished);
		gbuf_adds(text, tmp);
	}
	snprintf(tmp, sizeof tmp,
	         "waiting (blocked/interrupted/stalled;"
	         " %d recent of %d):\n", nwr, nw);
	gbuf_adds(text, tmp);
	if (!nw)
		gbuf_adds(text, "  (none)\n");
	for (i = 0; i < nw && i < limit; i++) {
		snprintf(tmp, sizeof tmp, "  %s %s %s since %s\n",
		         waiting[i].package, waiting[i].nvr,
		         waiting[i].state, waiting[i].finished);
		gbuf_adds(text, tmp);
	}
	snprintf(tmp, sizeof tmp,
	         "stale awaiting signing (PASS over 24h, unpublished;"
	         " %d recent of %d):\n", nar, na);
	gbuf_adds(text, tmp);
	if (!na)
		gbuf_adds(text, "  (none)\n");
	for (i = 0; i < na && i < limit; i++) {
		snprintf(tmp, sizeof tmp, "  %s %s %s since %s\n",
		         awaiting[i].package, awaiting[i].nvr,
		         awaiting[i].state, awaiting[i].finished);
		gbuf_adds(text, tmp);
	}
	snprintf(tmp, sizeof tmp,
	         "stale (recipe moved on; for reference, not outstanding;"
	         " %d shown):\n", ns);
	gbuf_adds(text, tmp);
	if (!ns)
		gbuf_adds(text, "  (none)\n");
	for (i = 0; i < ns && i < limit; i++) {
		snprintf(tmp, sizeof tmp, "  %s %s %s since %s (%s)\n",
		         stale[i].package, stale[i].nvr,
		         stale[i].state, stale[i].finished,
		         stale[i].detail);
		gbuf_adds(text, tmp);
	}
	gbuf_adds(text, "full attempt history: package_build_history; full journal: total_recall\n");
	/* Linked user tasks: checkpoints name their package explicitly;
	 * anything else would be an invented association, so unlinked
	 * bridge rows list without a task owner. */
	gbuf_adds(text, "linked checkpoints (exact package match only):\n");
	{
		sqlite3_stmt *cs = NULL;
		int any = 0;
		if (sqlite3_prepare_v2(jdb,
		    "SELECT id,label,title FROM checkpoints"
		    " WHERE status='active' AND package<>''"
		    " ORDER BY id DESC LIMIT 50", -1, &cs, NULL) == SQLITE_OK) {
			while (sqlite3_step(cs) == SQLITE_ROW) {
				const unsigned char *l =
				    sqlite3_column_text(cs, 1);
				const unsigned char *t =
				    sqlite3_column_text(cs, 2);
				char line[512];
				if (pkg && pkg[0]) {
					sqlite3_stmt *cp = NULL;
					int match = 0;
					if (sqlite3_prepare_v2(jdb,
					    "SELECT 1 FROM checkpoints"
					    " WHERE id=?1 AND package=?2",
					    -1, &cp, NULL) == SQLITE_OK) {
						sqlite3_bind_int64(cp, 1,
						    sqlite3_column_int64(cs, 0));
						sqlite3_bind_text(cp, 2, pkg, -1,
						    SQLITE_TRANSIENT);
						if (sqlite3_step(cp) == SQLITE_ROW)
							match = 1;
					}
					sqlite3_finalize(cp);
					if (!match)
						continue;
				}
				snprintf(line, sizeof line, "  [%lld] %s %s\n",
				         (long long)sqlite3_column_int64(cs, 0),
				         l ? (const char *)l : "",
				         t ? (const char *)t : "");
				gbuf_adds(text, line);
				any = 1;
			}
		}
		sqlite3_finalize(cs);
		if (!any)
			gbuf_adds(text, "  (none)\n");
	}
	if (qdb)
		sqlite3_close(qdb);
	else {
		gbuf_adds(text, "builder state unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
	}
	gbuf_adds(text, "[src: notes.db (local journal)");
	if (qok)
		gbuf_adds(text, " + build queue");
	gbuf_adds(text, "]\n");
	(void)tmp;
	return 0;
}

void bridge_instruction_line(gbuf *out) {
	sqlite3 *qdb = NULL;
	sqlite3_stmt *st = NULL;
	char dberr[256];
	long nf = 0, nfr = 0, nw = 0, na = 0;
	char tmp[256];
	qdb = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (!qdb)
		return;
	if (sqlite3_prepare_v2(qdb,
	    "SELECT state,started_at,queued_at,finished_at,published_at"
	    " FROM builds WHERE id IN"
	    " (SELECT max(id) FROM builds GROUP BY package)",
	    -1, &st, NULL) != SQLITE_OK) {
		sqlite3_close(qdb);
		return;
	}
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *s = sqlite3_column_text(st, 0);
		const unsigned char *started = sqlite3_column_text(st, 1);
		const unsigned char *queued = sqlite3_column_text(st, 2);
		const unsigned char *finished = sqlite3_column_text(st, 3);
		const unsigned char *published = sqlite3_column_text(st, 4);
		const char *state = s ? (const char *)s : "";
		const char *fin = finished ? (const char *)finished : "";
		if (strcmp(state, "FAIL") == 0 ||
		    strcmp(state, "INTERRUPTED") == 0) {
			nf++;
			if (bridge_recent(fin))
				nfr++;
		} else if (strcmp(state, "BLOCKED") == 0)
			nw++;
		else if (strcmp(state, "QUEUED") == 0 ||
		         strcmp(state, "BUILDING") == 0) {
			const char *ref = (started && started[0])
			    ? (const char *)started
			    : (queued ? (const char *)queued : "");
			if (parse_iso_age_s(ref) >= STALLED_AFTER_S)
				nw++;
		} else if (strcmp(state, "PASS") == 0 &&
		           (!published || !published[0])) {
			if (parse_iso_age_s(fin) >= AWAITING_AFTER_S)
				na++;
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(qdb);
	if (!nf && !nw && !na)
		return;
	/* Prioritised headline: needs-attention first (recent failed),
	 * complete counts preserved; the tool lists everything. */
	snprintf(tmp, sizeof tmp,
	         " Open builder obligations: %ld failed (%ld recent),"
	         " %ld waiting, %ld stale awaiting signing."
	         " See the obligations tool.",
	         nf, nfr, nw, na);
	gbuf_adds(out, tmp);
}

void bridge_startup_sync(void) {
	sqlite3 *jdb = NULL;
	char err[256];
	long imported = 0, scanned = 0;
	/* One bounded catch-up per initialize: imports builder history
	 * missed while this MCP was absent, then the cursor resumes next
	 * start. Strictly fail-open (missing/locked journal or queue
	 * yields silence) and time-bounded by bridge_run's page caps. */
	jdb = db_open_notes(err, sizeof err);
	if (!jdb)
		return;
	sqlite3_busy_timeout(jdb, 500);
	bridge_run(jdb, 100, &imported, &scanned);
	sqlite3_close(jdb);
}

void bridge_repo_newest(const char *pkg, char *nvr, size_t nn,
                        char *pub, size_t pn) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char path[4096], dberr[256];
	if (nn)
		nvr[0] = 0;
	if (pn)
		pub[0] = 0;
	if (!pkg || !pkg[0])
		return;
	if (snprintf(path, sizeof path, "%s/repository.db", path_repo_dir()) >= (int)sizeof path)
		return;
	db = db_open_ro(path, dberr, sizeof dberr);
	if (!db)
		return;
	if (sqlite3_prepare_v2(db,
	    "SELECT nvr,published_at FROM packages WHERE name=?1 AND is_newest=1",
	    -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW) {
			const unsigned char *n = sqlite3_column_text(st, 0);
			const unsigned char *p = sqlite3_column_text(st, 1);
			if (n)
				snprintf(nvr, nn, "%s", (const char *)n);
			if (p)
				snprintf(pub, pn, "%s", (const char *)p);
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
}

static int repo_has_nvr(const char *pkg, const char *nvr) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char path[4096], dberr[256];
	int hit = 0;
	if (!pkg || !pkg[0] || !nvr || !nvr[0])
		return 0;
	if (snprintf(path, sizeof path, "%s/repository.db", path_repo_dir()) >= (int)sizeof path)
		return 0;
	db = db_open_ro(path, dberr, sizeof dberr);
	if (!db)
		return 0;
	if (sqlite3_prepare_v2(db,
	    "SELECT 1 FROM packages WHERE name=?1 AND nvr=?2",
	    -1, &st, NULL) == SQLITE_OK) {
		char full[320];
		snprintf(full, sizeof full, "%s-%s", pkg, nvr);
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, full, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW)
			hit = 1;
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	return hit;
}

int bridge_recent(const char *iso) {
	long age = parse_iso_age_s(iso);
	return age >= 0 && age <= 7L * 86400L;
}

void bridge_effective(sqlite3 *qdb, const char *pkg, const char *req,
                      char *state, size_t sn, char *detail, size_t dn) {
	sqlite3_stmt *st = NULL;
	char row_state[16] = "", row_ver[64] = "", row_rev[16] = "";
	char row_pub[32] = "", row_fin[32] = "", row_start[32] = "", row_q[32] = "";
	char row_nvr[160] = "", repo_nvr[280] = "", repo_pub[32] = "";
	int have_row = 0;
	if (sn)
		state[0] = 0;
	if (dn)
		detail[0] = 0;
	if (!qdb) {
		snprintf(state, sn, "UNKNOWN");
		snprintf(detail, dn, "builder state unavailable");
		return;
	}
	if (!pkg || !pkg[0]) {
		snprintf(state, sn, "UNKNOWN");
		snprintf(detail, dn, "no package named");
		return;
	}
	if (sqlite3_prepare_v2(qdb,
	    "SELECT state,version,revision,published_at,finished_at,"
	    "started_at,queued_at FROM builds WHERE package=?1"
	    " ORDER BY id DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK) {
		snprintf(state, sn, "UNKNOWN");
		snprintf(detail, dn, "queue query failed");
		return;
	}
	sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *v;
		have_row = 1;
		v = sqlite3_column_text(st, 0);
		snprintf(row_state, sizeof row_state, "%s", v ? (const char *)v : "");
		v = sqlite3_column_text(st, 1);
		snprintf(row_ver, sizeof row_ver, "%s", v ? (const char *)v : "");
		v = sqlite3_column_text(st, 2);
		snprintf(row_rev, sizeof row_rev, "%s", v ? (const char *)v : "");
		v = sqlite3_column_text(st, 3);
		snprintf(row_pub, sizeof row_pub, "%s", v ? (const char *)v : "");
		v = sqlite3_column_text(st, 4);
		snprintf(row_fin, sizeof row_fin, "%s", v ? (const char *)v : "");
		v = sqlite3_column_text(st, 5);
		snprintf(row_start, sizeof row_start, "%s", v ? (const char *)v : "");
		v = sqlite3_column_text(st, 6);
		snprintf(row_q, sizeof row_q, "%s", v ? (const char *)v : "");
	}
	sqlite3_finalize(st);
	if (!have_row) {
		snprintf(state, sn, "NEVER BUILT");
		snprintf(detail, dn, "no queue history");
		return;
	}
	if (row_ver[0])
		snprintf(row_nvr, sizeof row_nvr, "%s-r%s", row_ver, row_rev);
	bridge_repo_newest(pkg, repo_nvr, sizeof repo_nvr, repo_pub, sizeof repo_pub);
	/* Exact-NVR publication wins over any row state: stale queue
	 * rows must never shadow satisfied revisions. */
	if (row_nvr[0] && repo_has_nvr(pkg, row_nvr)) {
		snprintf(state, sn, "PUBLISHED");
		snprintf(detail, dn, "succeeded (NVR %s live in repository)", row_nvr);
		return;
	}
	if (strcmp(row_state, "QUEUED") == 0 || strcmp(row_state, "BUILDING") == 0) {
		long age = parse_iso_age_s(row_start[0] ? row_start : row_q);
		/* Older BLOCKED attempts are relevant context even though
		 * the latest row wins: the daemon re-derives blocking on
		 * every scan, so a flip back is one scan away. */
		long nblocked = 0;
		sqlite3_stmt *bs = NULL;
		if (sqlite3_prepare_v2(qdb,
		    "SELECT COUNT(*) FROM builds WHERE package=?1"
		    " AND state='BLOCKED'", -1, &bs, NULL) == SQLITE_OK) {
			sqlite3_bind_text(bs, 1, pkg, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(bs) == SQLITE_ROW)
				nblocked = sqlite3_column_int64(bs, 0);
		}
		sqlite3_finalize(bs);
		if (age >= STALLED_AFTER_S) {
			snprintf(state, sn, "STALLED");
			if (nblocked > 0)
				snprintf(detail, dn,
				         "no progress; %ld older BLOCKED row%s on record",
				         nblocked, nblocked == 1 ? "" : "s");
			else
				snprintf(detail, dn,
				         "no progress, see package_build_status");
		} else {
			snprintf(state, sn, "%s", row_state);
			if (nblocked > 0)
				snprintf(detail, dn,
				         "latest attempt in progress; %ld older BLOCKED row%s on record",
				         nblocked, nblocked == 1 ? "" : "s");
			else
				snprintf(detail, dn, "latest attempt in progress");
		}
		return;
	}
	if (strcmp(row_state, "BLOCKED") == 0) {
		snprintf(state, sn, "BLOCKED");
		snprintf(detail, dn, "waiting on dependencies");
		return;
	}
	if (strcmp(row_state, "INTERRUPTED") == 0) {
		snprintf(state, sn, "INTERRUPTED");
		snprintf(detail, dn, "builder died mid-flight; retry explicitly");
		return;
	}
	if (strcmp(row_state, "FAIL") == 0) {
		if (req && req[0] && row_nvr[0] && strcmp(req, row_nvr) != 0) {
			snprintf(state, sn, "STALE");
			snprintf(detail, dn, "failed %s; recipe now requests %s",
			         row_nvr, req);
			return;
		}
		snprintf(state, sn, "FAILED");
		if (repo_nvr[0])
			snprintf(detail, dn, "requested %s failed; repo newest is %s",
			         row_nvr[0] ? row_nvr : "?", repo_nvr);
		else
			snprintf(detail, dn, "requested %s failed",
			         row_nvr[0] ? row_nvr : "?");
		return;
	}
	if (strcmp(row_state, "PASS") == 0) {
		if (row_pub[0] || (row_nvr[0] && repo_has_nvr(pkg, row_nvr))) {
			snprintf(state, sn, "PUBLISHED");
			if (repo_nvr[0] && row_nvr[0] && strcmp(repo_nvr + strlen(pkg) + 1, row_nvr) != 0)
				snprintf(detail, dn, "repo newest is %s", repo_nvr);
			else
				snprintf(detail, dn, "signed and published");
			return;
		}
		if (parse_iso_age_s(row_fin) >= AWAITING_AFTER_S) {
			snprintf(state, sn, "AWAITING SIGNING");
			snprintf(detail, dn, "built, stale: awaiting signing");
		} else {
			snprintf(state, sn, "AWAITING SIGNING");
			snprintf(detail, dn, "built, awaiting signing");
		}
		return;
	}
	snprintf(state, sn, "%s", row_state[0] ? row_state : "UNKNOWN");
	snprintf(detail, dn, "see package_build_status");
}
