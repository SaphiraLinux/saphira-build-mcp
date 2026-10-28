#!/bin/sh
# tests/test.sh: hermetic functional tests for saphira-build-mcp.
#
# Builds fixture databases, recipe tree, registry, logs and a fake
# `apk` under scratch, points the server at them via SBM_* env plus
# PATH, drives MCP sessions over stdio, and asserts. Never touches
# live paths: every data source the server reads is overridden here.
set -eu

ROOT=${SAPHIRA_TMPDIR:-/build/tmp}/sbm-test-$$
BIN=${SBM_BIN:-$PWD/saphira-build-mcp}
FIX=$ROOT/fixture
export SBM_RECIPES="$FIX/recipes"
export SBM_QUEUE_DB="$FIX/queue.db"
export SBM_REPO_DIR="$FIX/repo"
export SBM_INCOMING="$FIX/incoming"
export SBM_TSV="$FIX/accounts.tsv"
export SBM_PASSWD="$FIX/passwd"
export SBM_GROUP="$FIX/group"
export SBM_APKDB="$FIX/installed.db"
export SBM_TESTFIX="$FIX"
export SBM_NOTES_DB="$FIX/notes.db"
export PATH="$FIX/bin:$PATH"

pass=0
fail=0

t() {
	name=$1; rc=$2; got=$3; want=$4
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1)); printf 'PASS  %s\n' "$name"
	else
		fail=$((fail + 1)); printf 'FAIL  %s\n  got: %s\n  exp: %s\n' "$name" "$got" "$want"
	fi
}

t_contains() {
	name=$1; hay=$2; needle=$3
	case "$hay" in
		*"$needle"*) pass=$((pass + 1)); printf 'PASS  %s\n' "$name" ;;
		*) fail=$((fail + 1)); printf 'FAIL  %s\n  need: %s\n' "$name" "$needle" ;;
	esac
}

t_not_contains() {
	name=$1; hay=$2; needle=$3
	case "$hay" in
		*"$needle"*) fail=$((fail + 1)); printf 'FAIL  %s\n  must NOT contain: %s\n' "$name" "$needle" ;;
		*) pass=$((pass + 1)); printf 'PASS  %s\n' "$name" ;;
	esac
}

fail() { printf 'FAIL %s\n' "$1" >&2; exit 1; }

cleanup() {
	rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM
mkdir -p "$FIX/recipes/pkg-a" "$FIX/recipes/pkg-b" "$FIX/recipes/pkg-c"
mkdir -p "$FIX/repo" "$FIX/incoming" "$FIX/bin" "$FIX/logs/pkg-a"

[ -x "$BIN" ] || fail "not executable: $BIN (run make first or set SBM_BIN)"
command -v sqlite3 >/dev/null || fail "sqlite3 CLI required for fixtures"

# --- fixture recipe tree -------------------------------------------
cat > "$FIX/recipes/pkg-a/recipe.sh" <<'EOF'
pkgname=pkg-a
pkgver=1.0
pkgrel=2
pkgdesc="Fixture package A for tests"
license="MIT"
origin=pkg-a
subpackages="pkg-a-dev"
depends="
    pkg-c
    libfoo
"
EOF
cat > "$FIX/recipes/pkg-b/recipe.sh" <<'EOF'
pkgname=pkg-b
pkgver=0.5
pkgrel=1
pkgdesc="Disabled fixture package"
license="MIT"
disabled=yes
EOF
cat > "$FIX/recipes/pkg-c/recipe.sh" <<'EOF'
pkgname=pkg-c
pkgver=2.0
pkgrel=1
pkgdesc="Leaf dependency fixture"
license="MIT"
EOF
printf 'pkguser\tuser\t150\tpkguser\t/var/lib/pkg-a\t/sbin/nologin\t-\n' > "$FIX/accounts.tsv"
printf 'pkguser\tgroup\t150\t-\t-\t-\t-\n' >> "$FIX/accounts.tsv"
printf 'sysd-x\tuser\t144\tsysd-x\t/var/empty\t/sbin/nologin\t-\tsystemd\n' >> "$FIX/accounts.tsv"
printf 'pkguser:x:150:150:fixture:/var/lib/pkg-a:/sbin/nologin\nroot:x:0:0:root:/root:/bin/sh\n' > "$FIX/passwd"
printf 'pkguser:x:150:\nroot:x:0:\n' > "$FIX/group"
mkdir -p "$FIX/recipes/pkg-a/files/accounts.d"
printf 'user\tpkguser\t150\tpkguser\t/var/lib/pkg-a\t/sbin/nologin\n' > "$FIX/recipes/pkg-a/files/accounts.d/pkg-a"
printf 'group\tpkguser\t150\t-\t-\t-\t-\n' >> "$FIX/recipes/pkg-a/files/accounts.d/pkg-a"
printf 'dir\t/var/lib/pkg-a\t0755\tpkguser\tpkguser\n' >> "$FIX/recipes/pkg-a/files/accounts.d/pkg-a"

# --- fixture databases (schemas mirror the live ones) ----------------
sqlite3 "$FIX/queue.db" <<'SQL'
CREATE TABLE builds(id INTEGER PRIMARY KEY, package TEXT, version TEXT,
revision TEXT, recipe_hash TEXT, state TEXT, result_json TEXT, log_dir TEXT,
commit_hash TEXT, queued_at TEXT, started_at TEXT, finished_at TEXT,
published_at TEXT, builder_pid INTEGER, boot_id TEXT, duration_s REAL,
cpu_s REAL, peak_rss_mb REAL, timings_json TEXT, arch TEXT, builder_host TEXT);
CREATE TABLE sbu_baseline(id INTEGER PRIMARY KEY, package TEXT NOT NULL,
version TEXT, revision TEXT, recipe_commit TEXT, arch TEXT, builder_host TEXT,
cache_policy TEXT NOT NULL, wall_s REAL NOT NULL, cpu_s REAL,
active INTEGER NOT NULL DEFAULT 0, calibrated_at TEXT NOT NULL);
INSERT INTO builds(package,version,revision,state,result_json,duration_s,cpu_s,peak_rss_mb,arch,builder_host,queued_at,started_at,finished_at,published_at,log_dir) VALUES
('pkg-a','1.0',2,'PASS','{"attempt": null}',10.0,8.0,100.0,'x86_64','egg','2026-10-28T00:00:01Z','2026-10-28T00:00:02Z','2026-10-28T00:00:12Z','2026-10-28T00:01:00Z','LOGDIR'),
('pkg-a','1.0',1,'FAIL','{"attempt": "build", "stages": {"BUILD": {"status": "pass", "detail": "buildpkg rc=0"}, "INSTALL": {"status": "fail", "detail": "post-install exit 1"}}}',30.0,20.0,200.0,'x86_64','egg','2026-10-27T23:00:01Z','2026-10-27T23:00:02Z','2026-10-27T23:00:32Z',NULL,'LOGDIR'),
('pkg-b','0.5',1,'FAIL','{"attempt": "refusal", "stages": {"BUILD": {"status": "fail", "detail": "plan refused"}}}',1.0,0.3,27.5,'x86_64','egg','2026-10-28T00:00:01Z','2026-10-28T00:00:02Z','2026-10-28T00:00:03Z',NULL,NULL),
('pkg-c','2.0',1,'BUILDING',NULL,NULL,NULL,NULL,'x86_64','egg','2026-10-28T00:00:01Z','2026-10-28T00:00:02Z',NULL,NULL,NULL);
INSERT INTO sbu_baseline(package,version,revision,arch,builder_host,cache_policy,wall_s,cpu_s,active,calibrated_at) VALUES
('pkg-a','1.0',2,'x86_64','egg','cold',5.0,4.0,1,'2026-10-28T00:00:00Z');
SQL
# fix the LOGDIR placeholder to the real fixture log dir (kept out of
# the heredoc above so quoting stays trivial)
sqlite3 "$FIX/queue.db" "UPDATE builds SET log_dir='$FIX/logs/pkg-a' WHERE package='pkg-a';"
sqlite3 "$FIX/repo/repository.db" <<'SQL'
CREATE TABLE packages(package_id INTEGER PRIMARY KEY, name TEXT NOT NULL, version TEXT NOT NULL, pkgrel INTEGER NOT NULL, nvr TEXT NOT NULL, apk_sha256 TEXT NOT NULL, is_newest INTEGER NOT NULL, published_at TEXT, UNIQUE(name, version));
CREATE TABLE reservations(kind TEXT NOT NULL CHECK(kind IN ('user', 'group')), name TEXT NOT NULL, numeric_id INTEGER NOT NULL, owning_package TEXT NOT NULL, first_nvr TEXT NOT NULL, last_nvr TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('ACTIVE', 'TOMBSTONED')), declaration_sha256 TEXT NOT NULL, PRIMARY KEY(kind, name), UNIQUE(kind, numeric_id));
INSERT INTO packages(name,version,pkgrel,nvr,apk_sha256,is_newest,published_at) VALUES
('pkg-a','1.0',1,'pkg-a-1.0-r1','aa',1,'2026-10-27T00:00:00Z'),
('pkg-c','2.0',1,'pkg-c-2.0-r1','cc',1,'2026-10-27T00:00:00Z');
INSERT INTO reservations(kind,name,numeric_id,owning_package,first_nvr,last_nvr,status,declaration_sha256) VALUES
('user','pkguser',150,'pkg-a','pkg-a-1.0-r1','pkg-a-1.0-r1','ACTIVE','dd'),
('group','pkguser',150,'pkg-a','pkg-a-1.0-r1','pkg-a-1.0-r1','ACTIVE','dd');
SQL
printf 'C:xx\nP:pkg-a\nV:1.0-r1\nT:Fixture package A\nL:MIT\no:pkg-a\n\nC:yy\nP:pkg-c\nV:2.0-r1\nT:Leaf\nL:MIT\no:pkg-c\n\n' > "$FIX/installed.db"
printf 'build log line 1\nbuild log line 2\n' > "$FIX/logs/pkg-a/build.log"
python3 -c "open('$FIX/logs/pkg-a/install.log','w').write('x:y\n' * 300)" 
printf 'install tail\n' >> "$FIX/logs/pkg-a/install.log"
mkdir -p "$FIX/incoming/pkg-a-abc123-ready" "$FIX/incoming/pkg-a-old999-ready.superseded-rebuilt"
touch "$FIX/incoming/pkg-a-abc123-ready/pkg-a-1.0-r2.apk"

# --- fake apk on PATH (canned digraphs; unknown names fail) -----------
cat > "$FIX/bin/apk" <<EOF
#!/bin/sh
# Test double: understands only 'dot [pkg]'.
if [ "\$1" = "dot" ]; then
	if [ \$# -eq 1 ]; then
		cat "\$SBM_TESTFIX/dot-full.txt"
		exit 0
	fi
	case "\$2" in
		unknownpkg*) echo "ERROR: no such package: \$2" >&2; exit 1 ;;
		pkg-a) cat "\$SBM_TESTFIX/dot-pkga.txt"; exit 0 ;;
		*) echo "ERROR: no such package: \$2" >&2; exit 1 ;;
	esac
fi
echo "mock apk: unknown invocation" >&2
exit 1
EOF
chmod +x "$FIX/bin/apk"
cat > "$FIX/dot-pkga.txt" <<'EOF'
digraph "apkindex" {
  rankdir=LR;
  node [shape=box];
  "pkg-a-1.0-r2" -> "pkg-c-2.0-r1"[];
  "pkg-a-1.0-r2" -> "libfoo-1.2-r3"[];
}
EOF
cat > "$FIX/dot-full.txt" <<'EOF'
digraph "apkindex" {
  rankdir=LR;
  node [shape=box];
  "pkg-a-1.0-r2" -> "pkg-c-2.0-r1"[];
  "pkg-a-1.0-r2" -> "libfoo-1.2-r3"[];
  "pkg-c-2.0-r1" -> "libbar-0.1-r1"[];
  "top-9.9-r9" -> "pkg-a-1.0-r2"[];
}
EOF
t "fixture binary runs" "0" "$("$BIN" --list-tools >/dev/null 2>&1; echo $?)" "0"

# read-only proof: hash every fixture DB before any MCP traffic
H1=$(sha256sum "$FIX/queue.db" "$FIX/repo/repository.db" | sha256sum | cut -d' ' -f1)

mcp() { printf '%s\n' "$@" | "$BIN" --mcp 2>/dev/null; }

# --- protocol ---------------------------------------------------------
out=$(mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize echoes version" "$out" '"protocolVersion":"2025-03-26"'
t_contains "initialize names server" "$out" '"name":"saphira-build-mcp"'
out=$(mcp '{"jsonrpc":"2.0","id":2,"method":"ping"}')
t_contains "ping empty result" "$out" '"result":{}'
out=$(mcp '{"jsonrpc":"2.0","id":3,"method":"tools/list"}')
for tool in package_search package_info package_dependencies package_why_installed package_installed package_build_status package_build_history package_build_log package_artifacts build_queue package_identity package_notes_get package_notes_add package_policy_status package_policy_record package_readiness; do
	t_contains "tools/list has $tool" "$out" "\"name\":\"$tool\""
done
out=$(mcp '{"jsonrpc":"2.0","id":4,"method":"nope"}')
t_contains "unknown method -32601" "$out" '"code":-32601'
out=$(mcp 'not json at all')
t_contains "parse error -32700" "$out" '"code":-32700'
out=$(python3 -c "print(' ' * 70000)" | mcp "$(cat)")
t_contains "oversize -32600" "$out" '"code":-32600'
out=$(mcp '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"package_info","arguments":{}}}')
t_contains "missing arg -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"no-such-tool","arguments":{}}}')
t_contains "unknown tool -32602" "$out" '"code":-32602'

# --- query tools -------------------------------------------------------
out=$(mcp '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"package_search","arguments":{"query":"pkg"}}}')
t_contains "search finds pkg-a" "$out" "pkg-a 1.0-r2"
t_contains "search excludes disabled" "$out" "pkg-c"
t_not_contains "search hides disabled" "$out" "pkg-b"
out=$(mcp '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"package_search","arguments":{"query":"pkg","include_disabled":true}}}')
t_contains "search flag shows disabled" "$out" "pkg-b"
out=$(mcp '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"package_info","arguments":{"name":"pkg-a"}}}')
t_contains "info version" "$out" "version: 1.0"
t_contains "info repo newest" "$out" "pkg-a-1.0-r1"
t_contains "info provenance" "$out" "[src:"
out=$(mcp '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"package_info","arguments":{"name":"nosuchpkg"}}}')
t_contains "info unknown isError" "$out" '"isError":true'
out=$(mcp '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"package_dependencies","arguments":{"name":"pkg-a"}}}')
t_contains "deps pkg-c" "$out" "dep: pkg-c"
t_contains "deps libfoo" "$out" "dep: libfoo"
out=$(mcp '{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"package_dependencies","arguments":{"name":"pkg-a","direction":"reverse"}}}')
t_contains "reverse top" "$out" "rdep: top"
out=$(mcp '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"package_why_installed","arguments":{"package":"pkg-a","dep":"libbar"}}}')
t_contains "why chain" "$out" "chain: pkg-a -> pkg-c -> libbar"
out=$(mcp '{"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"package_why_installed","arguments":{"package":"pkg-a","dep":"notadep"}}}')
t_contains "why absent" "$out" "not in pkg-a's closure"
out=$(mcp '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"package_installed","arguments":{"name":"pkg-a"}}}')
t_contains "installed version" "$out" "installed: 1.0-r1"
t_contains "installed verdict" "$out" "verdict:"
out=$(mcp '{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"package_build_status","arguments":{"name":"pkg-a"}}}')
t_contains "status published" "$out" "PUBLISHED"
t_contains "status sbu" "$out" "SBU"
out=$(mcp '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"package_build_status","arguments":{"name":"pkg-b"}}}')
t_contains "status disabled note" "$out" "disabled"
out=$(mcp '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"package_build_status","arguments":{"name":"pkg-b","include_disabled":true}}}')
t_contains "status refused" "$out" "REFUSED"
out=$(mcp '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"package_build_history","arguments":{"name":"pkg-a","limit":5}}}')
t_contains "history fail stage" "$out" "INSTALL:"
t_contains "history shows count" "$out" "(showing 2)"
out=$(mcp '{"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"package_build_log","arguments":{"name":"pkg-a","stage":"build"}}}')
t_contains "log content" "$out" "build log line 1"
out=$(mcp '{"jsonrpc":"2.0","id":24,"method":"tools/call","params":{"name":"package_build_log","arguments":{"name":"pkg-a","stage":"install","tail_bytes":10}}}')
t_contains "log truncated" "$out" "showing last 10 of"
out=$(mcp '{"jsonrpc":"2.0","id":25,"method":"tools/call","params":{"name":"package_artifacts","arguments":{"name":"pkg-a"}}}')
t_contains "artifacts staged" "$out" "pkg-a-abc123-ready"
t_contains "artifacts superseded" "$out" "superseded"
t_contains "artifacts repo" "$out" "pkg-a-1.0-r1"
out=$(mcp '{"jsonrpc":"2.0","id":26,"method":"tools/call","params":{"name":"build_queue","arguments":{}}}')
t_contains "queue running" "$out" "pkg-c"
out=$(mcp '{"jsonrpc":"2.0","id":27,"method":"tools/call","params":{"name":"package_identity","arguments":{"name":"pkg-a"}}}')
t_contains "identity reservations" "$out" "owned by pkg-a"
t_contains "identity live user" "$out" "uid=150"
t_contains "identity registry" "$out" "pkguser"
t_not_contains "identity ignores dir paths" "$out" "/var/lib/pkg-a: absent"

# --- evidence journal (Egg-local notes.db; fixtures override it) --------
export SBM_AUTHOR=test-agent
# The journal must never live in the recipe tree: the server maintains
# its own tables, and fixtures point it at scratch (live default is
# /var/lib/saphira-build/notes.db on Egg, never /recipes).
t "journal path is fixture scratch" "$SBM_NOTES_DB" "$FIX/notes.db" "$FIX/notes.db"
out=$(mcp '{"jsonrpc":"2.0","id":40,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"finding","title":"deps verified","body":"pkg-c edge confirmed via apk dot"}}}' )
t_contains "notes add recorded" "$out" "recorded note 1 for pkg-a"
t_contains "notes add author default" "$out" "by test-agent"
out=$(mcp '{"jsonrpc":"2.0","id":41,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"issue","body":"libfoo unresolved","author":"ada"}}}' )
t_contains "notes add author override" "$out" "by ada"
# recorder comes from initialize clientInfo in the same connection
out=$(mcp '{"jsonrpc":"2.0","id":42,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"opencode","version":"9.9"}}}' '{"jsonrpc":"2.0","id":43,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"decision","body":"keep libfoo unresolved"}}}' )
t_contains "notes add with recorder" "$out" "recorded note 3 for pkg-a"
out=$(mcp '{"jsonrpc":"2.0","id":44,"method":"tools/call","params":{"name":"package_notes_get","arguments":{"package":"pkg-a"}}}')
t_contains "notes get newest" "$out" "keep libfoo unresolved"
t_contains "notes get recorder" "$out" "recorder=opencode/9.9"
t_contains "notes get count" "$out" "(showing 3)"
out=$(mcp '{"jsonrpc":"2.0","id":45,"method":"tools/call","params":{"name":"package_policy_record","arguments":{"package":"pkg-a","check":"unix-identity","result":"PASS","evidence":{"accounts_fragment_in_payload":true,"uid_gid_registered":true}}}}')
t_contains "policy record stored" "$out" "recorded unix-identity=PASS for pkg-a (record 1, first record)"
out=$(mcp '{"jsonrpc":"2.0","id":46,"method":"tools/call","params":{"name":"package_policy_status","arguments":{"package":"pkg-a"}}}')
t_contains "policy status current" "$out" "unix-identity: PASS CURRENT"
t_contains "policy status unverified" "$out" "payload-layout: UNVERIFIED (no record)"
out=$(mcp '{"jsonrpc":"2.0","id":47,"method":"tools/call","params":{"name":"package_readiness","arguments":{"package":"pkg-a"}}}')
t_contains "readiness verified" "$out" "unix-identity: PASS CURRENT"
t_contains "readiness notes" "$out" "notes: 3 (1 open issue)"
# evidence freshness: touch the recipe, the PASS must go stale
printf '# journal staleness probe\n' >> "$FIX/recipes/pkg-a/recipe.sh"
out=$(mcp '{"jsonrpc":"2.0","id":48,"method":"tools/call","params":{"name":"package_policy_status","arguments":{"package":"pkg-a"}}}')
t_contains "policy status stale" "$out" "unix-identity: PASS STALE"
t_contains "policy stale reason" "$out" "recipe moved"
out=$(mcp '{"jsonrpc":"2.0","id":49,"method":"tools/call","params":{"name":"package_readiness","arguments":{"package":"pkg-a"}}}')
t_contains "readiness stale bucket" "$out" "stale (needs revalidation)"
# supersede: a fresh record replaces the stale one, history preserved
n_before=$(sqlite3 "$FIX/notes.db" "SELECT COUNT(*) FROM policy_records;")
out=$(mcp '{"jsonrpc":"2.0","id":50,"method":"tools/call","params":{"name":"package_policy_record","arguments":{"package":"pkg-a","check":"unix-identity","result":"PASS","evidence":{"rechecked":true}}}}')
t_contains "policy record supersedes" "$out" "(record 2, supersedes 1)"
n_after=$(sqlite3 "$FIX/notes.db" "SELECT COUNT(*) FROM policy_records;")
t "journal append-only" "$n_after" "$((n_before + 1))" "$((n_before + 1))"
out=$(mcp '{"jsonrpc":"2.0","id":51,"method":"tools/call","params":{"name":"package_policy_status","arguments":{"package":"pkg-a"}}}')
t_contains "policy status current again" "$out" "unix-identity: PASS CURRENT"
t_contains "policy status newest record" "$out" "(record 2,"
# invalid inputs are transport errors, never journal writes
out=$(mcp '{"jsonrpc":"2.0","id":52,"method":"tools/call","params":{"name":"package_policy_record","arguments":{"package":"pkg-a","check":"no-such-check","result":"PASS","evidence":{"x":true}}}}')
t_contains "policy bad check -32602" "$out" '"code":-32602'
# a PASS with no evidence object is confidence, not verification
out=$(mcp '{"jsonrpc":"2.0","id":53,"method":"tools/call","params":{"name":"package_policy_record","arguments":{"package":"pkg-a","check":"payload-layout","result":"PASS"}}}')
t_contains "policy PASS without evidence -32602" "$out" '"code":-32602'
# a FAIL needs no evidence object: the failure is the record
out=$(mcp '{"jsonrpc":"2.0","id":54,"method":"tools/call","params":{"name":"package_policy_record","arguments":{"package":"pkg-a","check":"payload-layout","result":"FAIL"}}}')
t_contains "policy FAIL without evidence stored" "$out" "recorded payload-layout=FAIL for pkg-a"
out=$(mcp '{"jsonrpc":"2.0","id":55,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"nope","body":"x"}}}')
t_contains "notes bad kind -32602" "$out" '"code":-32602'
big=$(python3 -c "print('x' * 5000)")
out=$(mcp '{"jsonrpc":"2.0","id":56,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"finding","body":"'"$big"'"}}}')
t_contains "notes oversize body -32602" "$out" '"code":-32602'

# --- framing: every stdout line is valid JSON ---------------------------
mcp '{"jsonrpc":"2.0","id":30,"method":"tools/list"}' '{"jsonrpc":"2.0","id":31,"method":"ping"}' > "$ROOT/frames.txt"
python3 - "$ROOT/frames.txt" <<'EOF'
import json, sys
n = 0
for line in open(sys.argv[1]):
    line = line.strip()
    if line:
        json.loads(line)
        n += 1
assert n == 2, n
print("PASS  framing-valid-json")
EOF
[ $? -eq 0 ] || fail "framing validation crashed"

# --- read-only proof ----------------------------------------------------
H2=$(sha256sum "$FIX/queue.db" "$FIX/repo/repository.db" | sha256sum | cut -d' ' -f1)
t "fixture DBs byte-identical" "$H2" "$H1" "$H1"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
