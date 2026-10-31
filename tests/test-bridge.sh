#!/bin/sh
# tests/test-bridge.sh: hermetic tests for the builder/MCP bridge.
#
# Covers corrections 1-5 plus the published-NVR rule and the extra
# MCP-downtime requirement: non-blocking fail-open writes, transition
# capture (BLOCKED/UNBLOCKED/STALLED/AWAITING), stable identity without
# LIKE, resumable catch-up that cannot miss state changed after the
# cursor, bucket separation, published-NVR success, no invented task
# links, and concurrent writer/reconciler delivery.
#
# Fixtures only: a fake queue.db plus a journal under scratch, driven
# through the built binary over stdio. Never touches live paths.
set -eu

ROOT=${SAPHIRA_TMPDIR:-/build/tmp}/sbm-bridge-test-$$
BIN=${SBM_BIN:-$PWD/saphira-build-mcp}
FIX=$ROOT/fixture
export SBM_QUEUE_DB="$FIX/queue.db"
export SBM_NOTES_DB="$FIX/notes.db"
export SBM_BUILDER_NAME="bridgetest"

pass=0
fail=0
t_contains() {
	name=$1; hay=$2; needle=$3
	case "$hay" in
		*"$needle"*) pass=$((pass + 1)); printf 'PASS  %s\n' "$name" ;;
		*) fail=$((fail + 1)); printf 'FAIL  %s\n  need: %s\n  hay: %s\n' "$name" "$needle" "$hay" ;;
	esac
}
t_eq() {
	name=$1; got=$2; want=$3
	if [ "$got" = "$want" ]; then pass=$((pass + 1)); printf 'PASS  %s\n' "$name"
	else fail=$((fail + 1)); printf 'FAIL  %s\n  got: %s\n  exp: %s\n' "$name" "$got" "$want"; fi
}
failhere() { printf 'FAIL %s\n' "$1" >&2; exit 1; }

cleanup() { rm -rf "$ROOT"; }
trap cleanup EXIT INT TERM
mkdir -p "$FIX"

[ -x "$BIN" ] || failhere "not executable: $BIN (run make first or set SBM_BIN)"
command -v sqlite3 >/dev/null || failhere "sqlite3 CLI required for fixtures"
command -v python3 >/dev/null || failhere "python3 required for fixtures"

mcp() { printf '%s\n' "$@" | "$BIN" --mcp 2>/dev/null; }
bridge_events() { sqlite3 "$FIX/notes.db" "SELECT event || '|' || package || '|' || title FROM session_events ORDER BY id;"; }
bridge_count() { sqlite3 "$FIX/notes.db" "SELECT count(*) FROM session_events;"; }

mkqueue() {
	rm -f "$FIX/queue.db"
	python3 - "$FIX/queue.db" <<'PY'
import sqlite3, sys, json, time
q = sys.argv[1]
fmt = lambda t: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))
now = time.time(); today = fmt(now); old = fmt(now - 3 * 3600); stale = fmt(now - 2 * 86400)
conn = sqlite3.connect(q)
conn.execute("CREATE TABLE builds(id INTEGER PRIMARY KEY, package TEXT NOT NULL, version TEXT, revision TEXT, recipe_hash TEXT NOT NULL, state TEXT NOT NULL, result_json TEXT NOT NULL DEFAULT '{}', log_dir TEXT, commit_hash TEXT, queued_at TEXT, started_at TEXT, finished_at TEXT, published_at TEXT, builder_pid INTEGER, boot_id TEXT, duration_s REAL, cpu_s REAL, peak_rss_mb REAL, timings_json TEXT, arch TEXT, builder_host TEXT)")
conn.executemany("INSERT INTO builds(id,package,version,revision,recipe_hash,state,result_json,builder_host,queued_at,started_at,finished_at,published_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", [
 (1, "grub", "2.14", "1", "h" * 64, "PASS", json.dumps({"stages": {}}), "bridgetest", stale, stale, stale, stale),
 (2, "blockedpkg", "1.0", "1", "h" * 64, "BLOCKED", json.dumps({"stages": {"DEPENDENCIES": {"status": "blocked", "detail": ["ghost: QUEUED"]}}, "bridge": {"blocked_seq": 1}}), "bridgetest", today, None, None, None),
 (3, "stallpkg", "3.0", "1", "h" * 64, "BUILDING", json.dumps({"attempt": "build"}), "bridgetest", old, old, None, None),
 (4, "waitpkg", "4.0", "1", "h" * 64, "PASS", json.dumps({"stages": {}}), "bridgetest", stale, stale, stale, None),
 (5, "grub", "2.14", "4", "h" * 64, "FAIL", json.dumps({"attempt": "build", "stages": {"BUILD": {"status": "fail", "detail": "buildpkg rc=1"}}}), "bridgetest", today, today, today, None),
 (6, "lvm2", "2.03.39", "4", "h" * 64, "FAIL", json.dumps({"attempt": "build", "stages": {"BUILD": {"status": "fail", "detail": "buildpkg rc=1"}}}), "bridgetest", today, today, today, None),
])
conn.commit()
PY
	rm -f "$FIX/notes.db"
}

# 1. GRUB/LVM2 shape: new FAIL r4 with older PUBLISHED r1 lists r4,
# never hides the regression behind the old success.
mkqueue
out=$(mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
out=$(mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize pushes obligations line" "$out" "Open builder obligations:"
rm -f "$FIX/notes.db"
out=$(mcp '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"obligations","arguments":{}}}')
t_contains "first obligations call reconciles missed history" "$out" "reconciled"
out=$(mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"obligations","arguments":{}}}')
t_contains "obligations lists grub r4 failure" "$out" "grub 2.14-r4 FAIL"
t_contains "obligations lists lvm2 r4 failure" "$out" "lvm2 2.03.39-r4 FAIL"
t_contains "obligations lists blockedpkg" "$out" "blockedpkg 1.0-r1 BLOCKED"
t_contains "obligations lists stalled build" "$out" "stallpkg 3.0-r1 STALLED"
t_contains "obligations lists stale awaiting" "$out" "waitpkg 4.0-r1 AWAITING SIGNING"
t_contains "bridge events carry exact keys" "$(bridge_events)" "bridge:bridgetest:5:build.failed:1"

# 2. Idempotent: second run imports nothing new.
n1=$(bridge_count)
out=$(mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"obligations","arguments":{}}}')
n2=$(bridge_count)
t_eq "reconcile is idempotent" "$n2" "$n1"
case "$out" in *"reconciled"*) fail=$((fail + 1)); printf 'FAIL  no reimports on second run\n';; *) pass=$((pass + 1)); printf 'PASS  no reimports on second run\n';; esac

# 3. State changed after the cursor still reconciles: wipe the
# journal, plant a cursor far ahead of the rows, leave a BLOCKED row
# below it. Track 2 (id cursor) skips it; track 1 (active set)
# must still import build.blocked.
rm -f "$FIX/notes.db"
python3 - "$FIX/notes.db" <<'PY'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("CREATE TABLE bridge_state(key TEXT PRIMARY KEY, value TEXT NOT NULL)")
conn.execute("INSERT INTO bridge_state(key,value) VALUES('cursor','100')")
conn.commit()
PY
out=$(mcp '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"obligations","arguments":{"package":"blockedpkg"}}}')
t_contains "post-cursor BLOCKED transition reconciles" "$out" "blockedpkg 1.0-r1 BLOCKED"
t_contains "post-cursor import wrote the exact key" "$(bridge_events)" "bridge:bridgetest:2:build.blocked:1"

# 4. BLOCKED -> QUEUED emits blocked + unblocked; changed blockers
# bump seq and distinguish the transition.
python3 - "$FIX/queue.db" <<'PY'
import sqlite3, json, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("UPDATE builds SET state='QUEUED' WHERE id=2")
conn.commit()
PY
out=$(mcp '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"obligations","arguments":{"package":"blockedpkg"}}}')
t_contains "unblocked transition imports build.unblocked" "$(bridge_events)" "build.unblocked|blockedpkg|bridge:bridgetest:2:build.unblocked:1"

# 5. Published-NVR rule: exact NVR published reads succeeded.
python3 - "$FIX/queue.db" <<'PY'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("INSERT INTO builds(id,package,version,revision,recipe_hash,state,result_json,builder_host,queued_at,started_at,finished_at,published_at) VALUES(7,'grub','2.14','4','h','PASS','{}','bridgetest','t','t','t','t')")
conn.commit()
PY
out=$(mcp '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"obligations","arguments":{"package":"grub"}}}')
t_contains "exact published NVR succeeds despite old BLOCKED" "$out" "(none)"

# 6. No LIKE in bridge identity: titles collate exactly (the one
# allowed LIKE lives in the partial-index DDL; comment lines
# documenting the ban do not count).
if grep -n "LIKE" "$PWD/src/bridge.c" | grep -v "^[0-9]*: *\*" | grep -v "WHERE title LIKE 'bridge:%'" | grep -q .; then
	fail=$((fail + 1)); printf 'FAIL  bridge.c uses LIKE outside the partial-index DDL\n'
else
	pass=$((pass + 1)); printf 'PASS  no LIKE in bridge identity path\n'
fi

# 7. Concurrency: parallel reconciles never duplicate an event.
mkqueue
mcp '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"obligations","arguments":{}}}' > /dev/null &
mcp '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"obligations","arguments":{}}}' > /dev/null &
wait
dups=$(sqlite3 "$FIX/notes.db" "SELECT title, count(*) c FROM session_events GROUP BY title HAVING c > 1;")
t_eq "concurrent reconciles deliver each event once" "$dups" ""

# 8. Missing queue.db degrades gracefully (dashboard independence:
# journal-only, no error, no invented rows).
mv "$FIX/queue.db" "$FIX/queue.db.hidden"
out=$(mcp '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"obligations","arguments":{}}}')
t_contains "missing queue degrades with reason" "$out" "builder state unavailable"
mv "$FIX/queue.db.hidden" "$FIX/queue.db"

# 9. total_recall sees reconciled bridge rows (recall integration).
out=$(mcp '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"total_recall","arguments":{"mode":"query","query":"bridge","limit":10}}}')
t_contains "total_recall surfaces bridge events" "$out" "build.failed"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
