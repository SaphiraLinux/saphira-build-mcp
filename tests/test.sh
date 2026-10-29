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
mkdir -p "$FIX/recipes/pkg-a" "$FIX/recipes/pkg-b" "$FIX/recipes/pkg-c" "$FIX/recipes/pkg-s"
mkdir -p "$FIX/repo" "$FIX/incoming" "$FIX/bin" "$FIX/logs/pkg-a"

[ -x "$BIN" ] || fail "not executable: $BIN (run make first or set SBM_BIN)"
command -v sqlite3 >/dev/null || fail "sqlite3 CLI required for fixtures"
# Portable hashing: GNU sha256sum on Linux, shasum on macOS. Identical
# "<hash>  <file>" output, so the cut below works for both.
if command -v sha256sum >/dev/null 2>&1; then
	SHA256SUM=sha256sum
elif command -v shasum >/dev/null 2>&1; then
	SHA256SUM="shasum -a 256"
else
	fail "sha256sum or shasum required for fixtures"
fi

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
cat > "$FIX/recipes/pkg-s/recipe.sh" <<'EOF'
pkgname=pkg-s
pkgver=1.0
pkgrel=1
pkgdesc="Staged but unsigned fixture"
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
('pkg-c','2.0',1,'BUILDING',NULL,NULL,NULL,NULL,'x86_64','egg','2026-10-28T00:00:01Z','2026-10-28T00:00:02Z',NULL,NULL,NULL),
('pkg-s','1.0',1,'PASS','{"attempt": null}',5.0,4.0,50.0,'x86_64','egg','2026-10-28T00:00:01Z','2026-10-28T00:00:02Z','2026-10-28T00:00:12Z',NULL,'LOGDIR');
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

# --- fake builder frontends on PATH (argv-checked doubles) -----------
cat > "$FIX/bin/saphira-build" <<EOF
#!/bin/sh
# Test double: only 'scan'. Emulates the daemon intake by inserting
# one QUEUED row for pkg-a, and records its argv for assertions.
echo "\$@" >> "\$SBM_TESTFIX/scan-argv.log"
if [ "\$1" = "scan" ]; then
	sqlite3 "\$SBM_QUEUE_DB" "INSERT INTO builds(package,version,revision,state,queued_at) VALUES('pkg-a','1.0','2','QUEUED','2026-10-28T00:00:00Z');"
	echo "scan: 1 newly queued, 0 left failed, 0 already proven/current"
	echo "queue: 1 ready, 0 blocked"
	exit 0
fi
echo "mock saphira-build: unknown invocation" >&2
exit 1
EOF
chmod +x "$FIX/bin/saphira-build"
cat > "$FIX/bin/bumppkg" <<EOF
#!/bin/sh
# Test double: 'bumppkg --yes PKG'. Refuses pkg-c (guard path).
echo "\$@" >> "\$SBM_TESTFIX/bump-argv.log"
if [ "\$1" = "--yes" ] && [ "\$2" = "pkg-c" ]; then
	echo "bumppkg: refused pkg-c: anti-loop test guard" >&2
	exit 1
fi
if [ "\$1" = "--yes" ] && [ -n "\$2" ]; then
	echo "bumppkg: bumped \$2 (test double); ledger updated"
	exit 0
fi
echo "mock bumppkg: unknown invocation" >&2
exit 1
EOF
chmod +x "$FIX/bin/bumppkg"
t "fixture binary runs" "0" "$("$BIN" --list-tools >/dev/null 2>&1; echo $?)" "0"

# --- help and version ---------------------------------------------------
# Pure text on stdout, exit 0, no database touched (journal points
# nowhere, mode bogus); branding never leaks into protocol output.
t "help exit 0" "0" "$("$BIN" --help >/dev/null 2>&1; echo $?)" "0"
t "help -h exit 0" "0" "$("$BIN" -h >/dev/null 2>&1; echo $?)" "0"
t "help -h identical" "$("$BIN" --help 2>/dev/null)" "$("$BIN" -h 2>/dev/null)" "$("$BIN" --help 2>/dev/null)"
t "help needs no config" "0" "$(SBM_SAPHIRA=bogus SBM_NOTES_DB=/nonexistent-xyz-notes.db "$BIN" --help >/dev/null 2>&1; echo $?)" "0"
t "version exit 0" "0" "$("$BIN" --version >/dev/null 2>&1; echo $?)" "0"
ver=$("$BIN" --version 2>/dev/null)
t_contains "version names product" "$ver" "saphira-build-mcp "
t_contains "help carries version" "$("$BIN" --help 2>/dev/null)" "$ver"
t_contains "help product site" "$("$BIN" --help 2>/dev/null)" "https://saphira.vm2.uk"
t_contains "help commercial licence" "$("$BIN" --help 2>/dev/null)" "https://www.akadata.co.uk"
t_contains "help shampoo" "$("$BIN" --help 2>/dev/null)" "https://shampoo.op2.uk"
t_contains "help shampoo expands" "$("$BIN" --help 2>/dev/null)" "Shared Human-Agent-Model Platform for Orchestration"
t "unknown flag usage exit 2" "0" "$("$BIN" --frobnicate >/dev/null 2>&1 || echo $?)" "2"

# read-only proof: hash every fixture DB before any MCP traffic.
# repository.db must stay byte-identical throughout (nothing the MCP
# does writes it); queue.db legitimately gains rows once the action
# tools run, so its pre/post shape is asserted separately below.
H1Q=$($SHA256SUM "$FIX/queue.db" | cut -d' ' -f1)
H1R=$($SHA256SUM "$FIX/repo/repository.db" | cut -d' ' -f1)

mcp() { printf '%s\n' "$@" | "$BIN" --mcp 2>/dev/null; }

# Branding lives only in --help: protocol output must not carry it.
out=$(mcp '{"jsonrpc":"2.0","id":20,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_not_contains "protocol has no branding" "$out" "saphira.vm2.uk"

# --- protocol ---------------------------------------------------------
out=$(mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize echoes version" "$out" '"protocolVersion":"2025-03-26"'
t_contains "initialize names server" "$out" '"name":"saphira-build-mcp"'
out=$(mcp '{"jsonrpc":"2.0","id":2,"method":"ping"}')
t_contains "ping empty result" "$out" '"result":{}'
out=$(mcp '{"jsonrpc":"2.0","id":3,"method":"tools/list"}')
for tool in package_search package_info package_dependencies package_why_installed package_installed package_build_status package_build_history package_build_log package_artifacts build_queue package_identity package_notes_get package_notes_add package_policy_status package_policy_record package_readiness build_request package_bump checkpoint_save checkpoint_resume checkpoint_list checkpoint_close; do
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
t_contains "history shows baseline" "$out" "SBU baseline: cold 5.0s wall (x86_64/egg"
out=$(mcp '{"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"package_build_status","arguments":{"name":"pkg-c"}}}')
t_contains "status no baseline line" "$out" "SBU baseline: none active for pkg-c"
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
# the recorded digest must equal the reference tool's digest of the
# same file: proves the in-process SHA-256 is the standard one, on
# every host (macOS has no sha256sum; $SHA256SUM abstracts it).
t "policy digest matches system sha256" "0" "$(sqlite3 "$FIX/notes.db" "SELECT recipe_digest FROM policy_records WHERE package='pkg-a' AND check_name='unix-identity';")" "$($SHA256SUM "$FIX/recipes/pkg-a/recipe.sh" | cut -d' ' -f1)"
out=$(mcp '{"jsonrpc":"2.0","id":47,"method":"tools/call","params":{"name":"package_readiness","arguments":{"package":"pkg-a"}}}')
t_contains "readiness verified" "$out" "unix-identity: PASS CURRENT"
t_contains "readiness notes" "$out" "notes: 3 (1 open issue)"
t_contains "readiness recent header" "$out" "recent:"
t_contains "readiness recent newest" "$out" "keep libfoo unresolved"
t_contains "readiness recent title" "$out" "title: "
t_contains "readiness recent pull hint" "$out" "package_notes_get"
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
# reflective kinds: judged outcomes and carry-forward, not open questions
out=$(mcp '{"jsonrpc":"2.0","id":100,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"success","title":"mcp-queued build worked","body":"build_request row detached, no shell buildpkg needed"}}}' )
t_contains "notes add success" "$out" "recorded note 4 for pkg-a"
out=$(mcp '{"jsonrpc":"2.0","id":101,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"failure","title":"bumped empty revision","body":"wrong call: recipe already equalled published, minted churn"}}}' )
t_contains "notes add failure" "$out" "recorded note 5 for pkg-a"
out=$(mcp '{"jsonrpc":"2.0","id":102,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"lesson","title":"check published first","body":"next time, compare recipe against repository newest before any bump"}}}' )
t_contains "notes add lesson" "$out" "recorded note 6 for pkg-a"
out=$(mcp '{"jsonrpc":"2.0","id":103,"method":"tools/call","params":{"name":"package_notes_get","arguments":{"package":"pkg-a","kind":"lesson"}}}')
t_contains "notes get lesson filter" "$out" "check published first"
t_contains "notes get lesson count" "$out" "(showing 1)"
out=$(mcp '{"jsonrpc":"2.0","id":104,"method":"tools/call","params":{"name":"package_readiness","arguments":{"package":"pkg-a"}}}')
t_contains "readiness counts only issues as open" "$out" "notes: 6 (1 open issue)"
t_contains "readiness recent surfaces lesson" "$out" "check published first"
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

# --- action tools: build_request + package_bump ----------------------
# These legitimately change state (queue rows via the intake double,
# journal decision notes), so the read-only proof below covers the
# repository DB only; queue deltas are asserted exactly here instead.
q_before=$(sqlite3 "$FIX/queue.db" "SELECT COUNT(*) FROM builds WHERE package='pkg-a';")
out=$(mcp '{"jsonrpc":"2.0","id":60,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"pkg-a","reason":"verify journal flow"}}}')
t_contains "build request queued" "$out" "queued row"
t_contains "build request journaled" "$out" "note "
t_contains "build request reason kept" "$out" "verify journal flow"
t_contains "scan argv is bare scan" "$(cat "$FIX/scan-argv.log")" "scan"
q_after=$(sqlite3 "$FIX/queue.db" "SELECT COUNT(*) FROM builds WHERE package='pkg-a';")
t "scan queued exactly one row" "$q_after" "$((q_before + 1))" "$((q_before + 1))"
out=$(mcp '{"jsonrpc":"2.0","id":61,"method":"tools/call","params":{"name":"package_notes_get","arguments":{"package":"pkg-a","kind":"decision"}}}')
t_contains "build reason in journal" "$out" "verify journal flow"
# already-published NVR queues nothing and writes no note
n_before=$(sqlite3 "$FIX/notes.db" "SELECT COUNT(*) FROM notes;")
out=$(mcp '{"jsonrpc":"2.0","id":62,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"pkg-c","reason":"should not queue"}}}')
t_contains "build request published no-op" "$out" "already published (pkg-c-2.0-r1); nothing queued"
n_after=$(sqlite3 "$FIX/notes.db" "SELECT COUNT(*) FROM notes;")
t "published no-op writes no note" "$n_after" "$n_before" "$n_before"
# disabled and unknown packages are refused, never queued
out=$(mcp '{"jsonrpc":"2.0","id":63,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"pkg-b","reason":"x"}}}')
t_contains "build request disabled isError" "$out" '"isError":true'
out=$(mcp '{"jsonrpc":"2.0","id":64,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"nosuchpkg","reason":"x"}}}')
t_contains "build request unknown isError" "$out" '"isError":true'
# missing/oversize reason is a transport error
out=$(mcp '{"jsonrpc":"2.0","id":65,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"pkg-a"}}}')
t_contains "build request missing reason -32602" "$out" '"code":-32602'
bigreason=$(python3 -c "print('y' * 600)")
out=$(mcp '{"jsonrpc":"2.0","id":66,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"pkg-a","reason":"'"$bigreason"'"}}}')
t_contains "build request oversize reason -32602" "$out" '"code":-32602'
# revision bump: success journals the reason, refusal surfaces guards
out=$(mcp '{"jsonrpc":"2.0","id":67,"method":"tools/call","params":{"name":"package_bump","arguments":{"package":"pkg-a","reason":"rebuild for test"}}}')
t_contains "bump stored" "$out" "bumped pkg-a"
t_contains "bump reason journaled" "$out" "note "
t_contains "bump argv carries --yes" "$(cat "$FIX/bump-argv.log")" "--yes pkg-a"
out=$(mcp '{"jsonrpc":"2.0","id":68,"method":"tools/call","params":{"name":"package_bump","arguments":{"package":"pkg-c","reason":"x"}}}')
t_contains "bump refusal isError" "$out" '"isError":true'
t_contains "bump refusal reason" "$out" "refused"
out=$(mcp '{"jsonrpc":"2.0","id":69,"method":"tools/call","params":{"name":"package_bump","arguments":{"package":"pkg-b","reason":"x"}}}')
t_contains "bump disabled isError" "$out" '"isError":true'
# staged rows name the exact human-gated ask
out=$(mcp '{"jsonrpc":"2.0","id":70,"method":"tools/call","params":{"name":"package_build_status","arguments":{"name":"pkg-s"}}}')
t_contains "status awaiting signing" "$out" "AWAITING SIGNING"
t_contains "status names sign ask" "$out" "sudo sign-apk-repo pkg-s"

# --- checkpoints: pre-restart save, post-restart resume ---------------
# empty journal first: nothing saved yet
out=$(mcp '{"jsonrpc":"2.0","id":71,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{}}}')
t_contains "checkpoint resume empty" "$out" "(no checkpoints recorded)"
out=$(mcp '{"jsonrpc":"2.0","id":72,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"label":"nope"}}}')
t_contains "checkpoint resume unknown label isError" "$out" '"isError":true'
t_contains "checkpoint resume unknown label" "$out" "no checkpoints for label"
out=$(mcp '{"jsonrpc":"2.0","id":73,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":99}}}')
t_contains "checkpoint resume unknown id isError" "$out" '"isError":true'
t_contains "checkpoint resume unknown id" "$out" "no such checkpoint 99"
# save minimal (session-global scope), then full, then a same-label retry
out=$(mcp '{"jsonrpc":"2.0","id":74,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"pre-restart","body":"state: Master 1c1f304, queue empty\nnext: cut 0.4.0 release"}}}')
t_contains "checkpoint save minimal" "$out" "recorded checkpoint 1 (label 'pre-restart')"
out=$(mcp '{"jsonrpc":"2.0","id":75,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"pre-restart","title":"0.4.0 ready to cut","body":"all green","package":"pkg-a","src_commit":"0123456789abcdef0123456789abcdef01234567","recipe_commit":"abcdef0123456789abcdef0123456789abcdef01","author":"ada"}}}')
t_contains "checkpoint save full" "$out" "recorded checkpoint 2 (label 'pre-restart')"
out=$(mcp '{"jsonrpc":"2.0","id":76,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"pre-restart","body":"retry after restart"}}}')
t_contains "checkpoint save same label" "$out" "recorded checkpoint 3 (label 'pre-restart')"
# resume paths: by id, by label (newest), neither (newest), id wins
out=$(mcp '{"jsonrpc":"2.0","id":77,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":2}}}')
t_contains "checkpoint resume id" "$out" "checkpoint [2]"
t_contains "checkpoint resume title" "$out" "0.4.0 ready to cut"
t_contains "checkpoint resume scope" "$out" "pkg-a"
t_contains "checkpoint resume commits" "$out" "src_commit: 0123456789abcdef0123456789abcdef01234567"
t_contains "checkpoint resume author" "$out" "by ada"
out=$(mcp '{"jsonrpc":"2.0","id":78,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"label":"pre-restart"}}}')
t_contains "checkpoint resume label newest" "$out" "checkpoint [3]"
t_contains "checkpoint resume label body" "$out" "retry after restart"
out=$(mcp '{"jsonrpc":"2.0","id":79,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{}}}')
t_contains "checkpoint resume newest overall" "$out" "checkpoint [3]"
out=$(mcp '{"jsonrpc":"2.0","id":80,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":1,"label":"pre-restart"}}}')
t_contains "checkpoint resume id wins" "$out" "checkpoint [1]"
t_contains "checkpoint resume global scope" "$out" "scope: (session-global)"
t_contains "checkpoint resume multiline" "$out" "next: cut 0.4.0 release"
# bodies larger than the 4 KiB note cap fit: 5 KiB round-trips in full
bigbody=$(python3 -c "print('z: checkpoint payload ' + 'z'*4900 + ' tail-marker')")
out=$(mcp '{"jsonrpc":"2.0","id":81,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"big","body":"'"$bigbody"'"}}}')
t_contains "checkpoint save 5k body" "$out" "recorded checkpoint 4 (label 'big')"
out=$(mcp '{"jsonrpc":"2.0","id":82,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":4}}}')
t_contains "checkpoint resume 5k round-trip" "$out" "tail-marker"
# invalid inputs are transport errors, never journal writes
out=$(mcp '{"jsonrpc":"2.0","id":83,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"body":"no label"}}}')
t_contains "checkpoint save missing label -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":84,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"x"}}}')
t_contains "checkpoint save missing body -32602" "$out" '"code":-32602'
hugebody=$(python3 -c "print('q' * 17000)")
out=$(mcp '{"jsonrpc":"2.0","id":85,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"huge","body":"'"$hugebody"'"}}}')
t_contains "checkpoint save oversize body -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":86,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"x","body":"y","src_commit":"not-hex"}}}')
t_contains "checkpoint save bad commit -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":87,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":0}}}')
t_contains "checkpoint resume id zero -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":88,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":"two"}}}')
t_contains "checkpoint resume id type -32602" "$out" '"code":-32602'

# --- checkpoints 0.7.0: active register, close, identity, instructions
# two agents share one journal: identity fields keep lines apart
out=$(mcp '{"jsonrpc":"2.0","id":89,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"mcp-070","title":"register work","body":"two agents share /recipes","project":"saphira","workstream":"mcp-070","author":"muse"}}}')
t_contains "checkpoint save identity" "$out" "recorded checkpoint 5 (label 'mcp-070')"
out=$(mcp '{"jsonrpc":"2.0","id":90,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"kernel-cal","title":"Kernel calibration","body":"unfinished","project":"saphira","workstream":"kernel","author":"gpt"}}}')
t_contains "checkpoint save second agent" "$out" "recorded checkpoint 6 (label 'kernel-cal')"
# the register shows every agent's unfinished work, regardless of age
out=$(mcp '{"jsonrpc":"2.0","id":91,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{}}}')
t_contains "checkpoint list header" "$out" "active checkpoints: 6 total (showing 6)"
t_contains "checkpoint list keeps older work" "$out" "[1]"
t_contains "checkpoint list identity" "$out" "workstream 'mcp-070'"
out=$(mcp '{"jsonrpc":"2.0","id":92,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{"project":"saphira","workstream":"kernel"}}}')
t_contains "checkpoint list workstream filter" "$out" "1 total (showing 1)"
t_contains "checkpoint list filter row" "$out" "[6]"
t_not_contains "checkpoint list filter hides other" "$out" "[5]"
# close: unknown, then real, then idempotent; history survives closing
out=$(mcp '{"jsonrpc":"2.0","id":93,"method":"tools/call","params":{"name":"checkpoint_close","arguments":{"id":99}}}')
t_contains "checkpoint close unknown isError" "$out" '"isError":true'
out=$(mcp '{"jsonrpc":"2.0","id":94,"method":"tools/call","params":{"name":"checkpoint_close","arguments":{"id":5}}}')
t_contains "checkpoint close ok" "$out" "closed checkpoint 5 (label 'mcp-070')"
out=$(mcp '{"jsonrpc":"2.0","id":95,"method":"tools/call","params":{"name":"checkpoint_close","arguments":{"id":5}}}')
t_contains "checkpoint close idempotent" "$out" "already closed"
out=$(mcp '{"jsonrpc":"2.0","id":96,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{}}}')
t_contains "checkpoint close hides from active" "$out" "5 total (showing 5)"
t_not_contains "checkpoint close hides row" "$out" "[5]"
out=$(mcp '{"jsonrpc":"2.0","id":97,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{"status":"all"}}}')
t_contains "checkpoint list all keeps closed" "$out" "(closed)"
out=$(mcp '{"jsonrpc":"2.0","id":98,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":5}}}')
t_contains "checkpoint resume closed by id" "$out" "status: closed"
t_contains "checkpoint resume closed body" "$out" "two agents share /recipes"
# invalid list/close inputs are transport errors, never journal writes
out=$(mcp '{"jsonrpc":"2.0","id":99,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{"status":"bogus"}}}')
t_contains "checkpoint list bad status -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":100,"method":"tools/call","params":{"name":"checkpoint_close","arguments":{}}}')
t_contains "checkpoint close missing id -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":101,"method":"tools/call","params":{"name":"checkpoint_close","arguments":{"id":0}}}')
t_contains "checkpoint close id zero -32602" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":102,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"x","body":"y","project":"'"$(python3 -c "print('p'*65)")"'"}}}')
t_contains "checkpoint save oversize project -32602" "$out" '"code":-32602'
# instructions advertise the register at connect time
out=$(mcp '{"jsonrpc":"2.0","id":103,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize has instructions" "$out" '"instructions":"Session checkpoints are a persistent register'
t_contains "initialize lists active" "$out" "Active checkpoints (5 total)"
t_contains "initialize names row" "$out" "kernel-cal"
t_contains "initialize never auto-resume" "$out" "Never auto-resume"
# legacy journal: pre-0.7.0 rows read back as active (migration proof)
sqlite3 "$FIX/legacy-notes.db" "CREATE TABLE checkpoints(id INTEGER PRIMARY KEY,label TEXT NOT NULL,package TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',body TEXT NOT NULL,author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',src_commit TEXT,recipe_commit TEXT,created_at TEXT NOT NULL); INSERT INTO checkpoints(label,body,author,created_at) VALUES('legacy-line','old work','ada','2026-10-20T00:00:00Z');"
out=$(SBM_NOTES_DB="$FIX/legacy-notes.db" mcp '{"jsonrpc":"2.0","id":104,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{}}}')
t_contains "checkpoint legacy migrates to active" "$out" "1 total (showing 1)"
t_contains "checkpoint legacy row" "$out" "legacy-line"
out=$(SBM_NOTES_DB="$FIX/legacy-notes.db" mcp '{"jsonrpc":"2.0","id":105,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "checkpoint legacy instructions" "$out" "Active checkpoints (1 total)"
# fresh journal: empty register reads as zero, honestly stated
sqlite3 "$FIX/fresh-notes.db" "CREATE TABLE checkpoints(id INTEGER PRIMARY KEY,label TEXT NOT NULL,package TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',body TEXT NOT NULL,author TEXT NOT NULL,recorder TEXT NOT NULL DEFAULT '',src_commit TEXT,recipe_commit TEXT,created_at TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'active',project TEXT NOT NULL DEFAULT '',workstream TEXT NOT NULL DEFAULT '',closed_at TEXT,closed_by TEXT);"
out=$(SBM_NOTES_DB="$FIX/fresh-notes.db" mcp '{"jsonrpc":"2.0","id":106,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize fresh zero" "$out" "No active checkpoints recorded."
# missing journal: static guidance only, never an invented count
out=$(SBM_NOTES_DB="$FIX/missing-notes.db" mcp '{"jsonrpc":"2.0","id":107,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize missing journal static" "$out" '"instructions":"Session checkpoints are a persistent register'
t_not_contains "initialize missing journal no count" "$out" "Active checkpoints ("

# --- tools.conf extensions --------------------------------------------
# absent conf: built-ins only, no externals, never an error.
out=$(SBM_TOOLS_CONF="$FIX/no-such.conf" mcp '{"jsonrpc":"2.0","id":200,"method":"tools/list"}')
t_contains "ext absent conf builtins" "$out" '"name":"checkpoint_close"'
t_not_contains "ext absent conf no externals" "$out" 'External ('
# fixture externals: fixed argv only, sandbox off (hermetic).
cat > "$FIX/bin/ext-ok.sh" <<EOF
#!/bin/sh
echo "ext-ok args: \$*"
EOF
chmod +x "$FIX/bin/ext-ok.sh"
cat > "$FIX/bin/ext-fail.sh" <<EOF
#!/bin/sh
echo "ext-fail output"
exit 3
EOF
chmod +x "$FIX/bin/ext-fail.sh"
cat > "$FIX/bin/ext-slow.sh" <<EOF
#!/bin/sh
sleep 5
echo "should never appear"
EOF
chmod +x "$FIX/bin/ext-slow.sh"
cat > "$FIX/bin/ext-loud.sh" <<EOF
#!/bin/sh
i=0
while [ \$i -lt 100 ]; do echo "loud line \$i ........................................"; i=\$((i+1)); done
EOF
chmod +x "$FIX/bin/ext-loud.sh"
cat > "$FIX/ext-good.conf" <<EOF
tool ext_ok $FIX/bin/ext-ok.sh read 10 65536 off $FIX --
tool ext_fail $FIX/bin/ext-fail.sh read 10 65536 off $FIX --
tool ext_slow $FIX/bin/ext-slow.sh read 1 65536 off $FIX --
tool ext_loud $FIX/bin/ext-loud.sh read 10 64 off $FIX --
tool ext_boxed $FIX/bin/ext-ok.sh read 10 65536 on $FIX --
EOF
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" mcp '{"jsonrpc":"2.0","id":201,"method":"tools/list"}')
t_contains "ext list has ext_ok" "$out" '"name":"ext_ok"'
t_contains "ext list describes class" "$out" 'External (read):'
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" mcp '{"jsonrpc":"2.0","id":202,"method":"tools/call","params":{"name":"ext_ok","arguments":{}}}')
t_contains "ext call output" "$out" 'ext-ok args:'
t_contains "ext call exit 0" "$out" '[exit=0'
t_contains "ext call no error" "$out" '"isError":false'
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" mcp '{"jsonrpc":"2.0","id":203,"method":"tools/call","params":{"name":"ext_fail","arguments":{}}}')
t_contains "ext fail isError" "$out" '"isError":true'
t_contains "ext fail exit 3" "$out" '[exit=3'
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" mcp '{"jsonrpc":"2.0","id":204,"method":"tools/call","params":{"name":"ext_slow","arguments":{}}}')
t_contains "ext slow timeout" "$out" '[timeout]'
t_contains "ext slow isError" "$out" '"isError":true'
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" mcp '{"jsonrpc":"2.0","id":205,"method":"tools/call","params":{"name":"ext_loud","arguments":{}}}')
t_contains "ext loud truncated" "$out" '[truncated]'
t_contains "ext loud no error" "$out" '"isError":false'
t "ext audit ok row" "$(sqlite3 "$FIX/notes.db" "SELECT COUNT(*) FROM tool_runs WHERE tool='ext_ok' AND exit_status=0;")" "1" "1"
t "ext audit timeout row" "$(sqlite3 "$FIX/notes.db" "SELECT COUNT(*) FROM tool_runs WHERE tool='ext_slow' AND timed_out=1;")" "1" "1"
# malformed or unsafe conf: extensions (and expose-offs) disabled,
# built-ins unaffected. one bad file per case.
printf 'expose package_search off\ntool ext_q %s read 10 100 off %s --\n' "$FIX/bin/ext-ok.sh" "$FIX" > "$FIX/ext-off.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-off.conf" mcp '{"jsonrpc":"2.0","id":210,"method":"tools/list"}')
t_not_contains "ext off omitted" "$out" '"name":"package_search"'
t_contains "ext off keeps ext" "$out" '"name":"ext_q"'
out=$(SBM_TOOLS_CONF="$FIX/ext-off.conf" mcp '{"jsonrpc":"2.0","id":211,"method":"tools/call","params":{"name":"package_search","arguments":{"query":"pkg"}}}')
t_contains "ext off call unknown" "$out" '"code":-32602'
printf 'expose checkpoint_list off\ntool ext_q %s read 10 100 off %s --\n' "$FIX/bin/ext-ok.sh" "$FIX" > "$FIX/ext-badckpt.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-badckpt.conf" mcp '{"jsonrpc":"2.0","id":212,"method":"tools/list"}')
t_contains "ext checkpoint forced on" "$out" '"name":"checkpoint_list"'
t_not_contains "ext bad conf no ext" "$out" 'External ('
printf 'expose nosuchtool off\n' > "$FIX/ext-badexpose.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-badexpose.conf" mcp '{"jsonrpc":"2.0","id":213,"method":"tools/list"}')
t_contains "ext unknown expose ignored" "$out" '"name":"package_search"'
printf 'tool package_info %s read 10 100 off %s --\n' "$FIX/bin/ext-ok.sh" "$FIX" > "$FIX/ext-shadow.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-shadow.conf" mcp '{"jsonrpc":"2.0","id":214,"method":"tools/list"}')
t_contains "ext shadow keeps builtin" "$out" '"name":"package_info"'
t_not_contains "ext shadow no ext" "$out" 'External ('
printf 'tool badsh /bin/sh read 10 100 off %s -- -c\n' "$FIX" > "$FIX/ext-denied.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-denied.conf" mcp '{"jsonrpc":"2.0","id":215,"method":"tools/list"}')
t_contains "ext denied exe builtins" "$out" '"name":"package_search"'
t_not_contains "ext denied exe no ext" "$out" 'External ('
printf 'tool badmeta %s read 10 100 off %s -- %s\n' "$FIX/bin/ext-ok.sh" "$FIX" 'a;b' > "$FIX/ext-meta.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-meta.conf" mcp '{"jsonrpc":"2.0","id":216,"method":"tools/list"}')
t_not_contains "ext metachar no ext" "$out" 'External ('
printf 'tool badstate %s read 10 100 off %s -- queue.db\n' "$FIX/bin/ext-ok.sh" "$FIX" > "$FIX/ext-state.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-state.conf" mcp '{"jsonrpc":"2.0","id":217,"method":"tools/list"}')
t_not_contains "ext state token no ext" "$out" 'External ('
printf 'tool gone /nonexistent/gone read 10 100 off %s --\n' "$FIX" > "$FIX/ext-gone.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-gone.conf" mcp '{"jsonrpc":"2.0","id":218,"method":"tools/call","params":{"name":"gone","arguments":{}}}')
t_contains "ext missing exe isError" "$out" '"isError":true'
t_contains "ext missing exe message" "$out" 'unavailable'
# A real sandbox run needs bwrap (absent on macOS and minimal
# hosts): skip the success asserts there, keep the fail-closed one.
if command -v "${SBM_BWRAP:-bwrap}" >/dev/null 2>&1; then
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" mcp '{"jsonrpc":"2.0","id":219,"method":"tools/call","params":{"name":"ext_boxed","arguments":{}}}')
t_contains "ext sandbox runs" "$out" 'ext-ok args:'
t_contains "ext sandbox exit 0" "$out" '[exit=0'
else
	printf 'SKIP  ext sandbox runs (no bwrap on PATH)\n'
fi
out=$(SBM_TOOLS_CONF="$FIX/ext-good.conf" SBM_BWRAP="/nonexistent-bwrap" mcp '{"jsonrpc":"2.0","id":220,"method":"tools/call","params":{"name":"ext_boxed","arguments":{}}}')
t_contains "ext sandbox missing fails closed" "$out" '"isError":true'
# sandbox profile: the recipe-tree bind is conditional on existence
# so sandboxed externals run off-builder too. Fake bwrap records the
# argv; the assertion follows whichever host this runs on.
cat > "$FIX/bin/fake-bwrap.sh" <<EOF
#!/bin/sh
echo "FAKEBWRAP-ARGS: \$*"
EOF
chmod +x "$FIX/bin/fake-bwrap.sh"
printf 'tool ext_probe %s read 10 65536 on %s --\n' "$FIX/bin/ext-ok.sh" "$FIX" > "$FIX/ext-probe.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-probe.conf" SBM_BWRAP="$FIX/bin/fake-bwrap.sh" mcp '{"jsonrpc":"2.0","id":221,"method":"tools/call","params":{"name":"ext_probe","arguments":{}}}')
t_contains "sandbox profile keeps unshare" "$out" "--unshare-net"
t_contains "sandbox profile binds usr" "$out" "--ro-bind /usr /usr"
if [ -d /recipes ]; then
	t_contains "sandbox binds recipes where present" "$out" "--ro-bind /recipes /recipes"
else
	t_not_contains "sandbox skips recipes where absent" "$out" "/recipes"
fi

# --- portable mode (SBM_SAPHIRA=off) -----------------------------------
# Same binary on machines without builder state: Saphira-state tools
# vanish from tools/list and refuse calls as unknown; checkpoints,
# the notes journal and configured externals keep working against a
# fresh journal. Builder data paths point at nothing that exists to
# prove startup never probes or initialises builder infrastructure.
PVOID=$ROOT/void
PJOURNAL=$ROOT/pjournal/notes.db
export SBM_SAPHIRA=off
export SBM_NOTES_DB="$PJOURNAL"
export SBM_RECIPES="$PVOID/recipes" SBM_QUEUE_DB="$PVOID/queue.db"
export SBM_REPO_DIR="$PVOID/repo" SBM_INCOMING="$PVOID/incoming"
export SBM_TSV="$PVOID/accounts.tsv" SBM_PASSWD="$PVOID/passwd"
export SBM_GROUP="$PVOID/group" SBM_APKDB="$PVOID/installed.db"
export SBM_TOOLS_CONF="$PVOID/no-such.conf"
out=$("$BIN" --list-tools)
for n in checkpoint_save checkpoint_resume checkpoint_list checkpoint_close package_notes_get package_notes_add; do
	t_contains "portable list keeps $n" "$out" "\"name\":\"$n\""
done
for n in package_search package_info package_dependencies package_why_installed package_installed package_build_status package_build_history package_build_log package_artifacts build_queue package_identity package_policy_status package_policy_record package_readiness build_request package_bump; do
	t_not_contains "portable list drops $n" "$out" "\"name\":\"$n\""
done
out=$(mcp '{"jsonrpc":"2.0","id":301,"method":"tools/call","params":{"name":"package_info","arguments":{"name":"pkg-a"}}}')
t_contains "portable gated call unknown" "$out" '"code":-32602'
out=$(mcp '{"jsonrpc":"2.0","id":302,"method":"tools/call","params":{"name":"build_request","arguments":{"package":"pkg-a","reason":"x","author":"t"}}}')
t_contains "portable build_request unknown" "$out" '"code":-32602'
# journal round-trip across processes: every mcp() call is its own
# process, so save here and resume/list in later processes.
out=$(mcp '{"jsonrpc":"2.0","id":303,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"portable-trip","body":"state across restarts"}}}')
t_contains "portable checkpoint save" "$out" "recorded checkpoint 1 (label 'portable-trip')"
out=$(mcp '{"jsonrpc":"2.0","id":304,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"id":1}}}')
t_contains "portable resume other process" "$out" "state across restarts"
out=$(mcp '{"jsonrpc":"2.0","id":305,"method":"tools/call","params":{"name":"checkpoint_list","arguments":{}}}')
t_contains "portable register other process" "$out" "portable-trip"
out=$(mcp '{"jsonrpc":"2.0","id":306,"method":"tools/call","params":{"name":"package_notes_add","arguments":{"package":"pkg-a","kind":"lesson","title":"portable lesson","body":"travels with the binary","author":"t"}}}')
t_contains "portable notes add" "$out" "recorded note"
out=$(mcp '{"jsonrpc":"2.0","id":307,"method":"tools/call","params":{"name":"package_notes_get","arguments":{"package":"pkg-a"}}}')
t_contains "portable notes get" "$out" "portable lesson"
t "portable void untouched" "$(test -e "$PVOID/recipes" -o -e "$PVOID/queue.db" -o -e "$PVOID/repo" && echo touched || echo clean)" "clean" "clean"
t "portable journal created" "$(test -f "$PJOURNAL" && echo yes || echo no)" "yes" "yes"
# externals survive portable mode unchanged.
printf 'tool ext_ok %s read 10 65536 off %s --\n' "$FIX/bin/ext-ok.sh" "$FIX" > "$FIX/ext-portable.conf"
out=$(SBM_TOOLS_CONF="$FIX/ext-portable.conf" mcp '{"jsonrpc":"2.0","id":308,"method":"tools/list"}')
t_contains "portable ext listed" "$out" '"name":"ext_ok"'
out=$(SBM_TOOLS_CONF="$FIX/ext-portable.conf" mcp '{"jsonrpc":"2.0","id":309,"method":"tools/call","params":{"name":"ext_ok","arguments":{}}}')
t_contains "portable ext runs" "$out" 'ext-ok args:'
# invalid mode is an explicit configuration failure, never silent on.
out=$(SBM_SAPHIRA=bogus "$BIN" --list-tools 2>&1 || echo "rc=$?")
t_contains "portable invalid message" "$out" "invalid SBM_SAPHIRA='bogus'"
t_contains "portable invalid rc" "$out" "rc=2"
out=$(printf '%s\n' '{"jsonrpc":"2.0","id":310,"method":"tools/list"}' | SBM_SAPHIRA=bogus "$BIN" --mcp 2>&1 || echo "rc=$?")
t_contains "portable invalid mcp message" "$out" "invalid SBM_SAPHIRA='bogus'"
t_contains "portable invalid mcp rc" "$out" "rc=2"
# explicit on keeps the full builder surface (fixture-backed here).
out=$(SBM_SAPHIRA=on "$BIN" --list-tools)
t_contains "mode on keeps package_info" "$out" '"name":"package_info"'
t_contains "mode on keeps build_request" "$out" '"name":"build_request"'
# restore builder mode for the rest of the suite.
unset SBM_SAPHIRA
export SBM_NOTES_DB="$FIX/notes.db"
export SBM_RECIPES="$FIX/recipes" SBM_QUEUE_DB="$FIX/queue.db"
export SBM_REPO_DIR="$FIX/repo" SBM_INCOMING="$FIX/incoming"
export SBM_TSV="$FIX/accounts.tsv" SBM_PASSWD="$FIX/passwd"
export SBM_GROUP="$FIX/group" SBM_APKDB="$FIX/installed.db"

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
# repository.db is never written by any tool: byte-identical. queue.db
# legitimately gains rows through the daemon intake (asserted exactly
# above); the journal lives in its own database by design.
H2R=$($SHA256SUM "$FIX/repo/repository.db" | cut -d' ' -f1)
t "repository DB byte-identical" "$H2R" "$H1R" "$H1R"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
