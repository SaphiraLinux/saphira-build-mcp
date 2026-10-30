#!/bin/sh
# docs/smoke.sh - documentation smoke test for saphira-build-mcp.
#
# Every runnable claim in docs/*.md is re-executed here so a future
# protocol or tool change cannot silently invalidate the examples.
# Hermetic: temporary journal, no builds, no signing, no publication,
# no writes outside the scratch dir. POSIX sh + python3 + curl.
#
# Usage: sh docs/smoke.sh   (from the repository root)
set -u
BIN=./saphira-build-mcp
TMP=$(mktemp -d "${TMPDIR:-/tmp}/sbm-docs-smoke.XXXXXX") || exit 1
trap 'kill $BRIDGEPID 2>/dev/null; rm -rf "$TMP"' EXIT INT TERM
BRIDGEPID=""

pass() { echo "SMOKE-PASS: $1"; }
fail() { echo "SMOKE-FAIL: $1"; exit 1; }

export SBM_NOTES_DB="$TMP/notes.db"
export SBM_TOOLS_CONF=/nonexistent-tools.conf
export SBM_AUTHOR=smoke

names() { # $1 = extra env assignments, $2 = expected count
  env $1 "$BIN" --list-tools 2>/dev/null | python3 -c \
    "import json,sys; print('\n'.join(t['name'] for t in json.load(sys.stdin)['tools']))"
}
count() { names "$1" | wc -l | tr -d ' '; }

# 1. capability matrices (docs/mcp-protocol.md, docs/tools.md footer)
[ "$(count '')" = "23" ] || fail "default tool count"
names '' | grep -qx total_recall || fail "total_recall missing"
[ "$(count 'SBM_BUILD=off')" = "21" ] || fail "build-off count"
names 'SBM_BUILD=off' | grep -qx build_request && fail "build_request leaks with build off"
[ "$(count 'SBM_SAPHIRA=off')" = "12" ] || fail "portable count"
LOCKED=$(names 'SBM_SAPHIRA=off SBM_BUILD=off')
[ "$(printf '%s\n' "$LOCKED" | wc -l | tr -d ' ')" = "10" ] || fail "locked-down count"
for t in checkpoint_save checkpoint_resume checkpoint_list checkpoint_close \
  package_notes_get package_notes_add total_recall package_policy_status \
  package_policy_record package_readiness; do
  printf '%s\n' "$LOCKED" | grep -qx "$t" || fail "locked-down missing $t"
done
pass "capability matrices 23/21/12/10"
"$BIN" --list-tools >/dev/null 2>&1 || fail "--list-tools exit"
SBM_SAPHIRA=bogus "$BIN" --list-tools >/dev/null 2>&1 && fail "bogus SBM_SAPHIRA accepted"
SBM_BUILD=bogus "$BIN" --list-tools >/dev/null 2>&1 && fail "bogus SBM_BUILD accepted"
pass "invalid capability values fail"

# 2. malformed conf disables extensions, keeps built-ins (docs/external-tools.md)
printf 'this is not a directive\n' > "$TMP/bad.conf"
[ "$(SBM_TOOLS_CONF="$TMP/bad.conf" "$BIN" --list-tools 2>/dev/null |
  python3 -c "import json,sys; print(len(json.load(sys.stdin)['tools']))")" = "23" ] \
  || fail "malformed conf changed built-ins"
pass "malformed conf keeps built-ins only"

# 3. valid conf: expose-off + one read-only git external ( require git )
if command -v git >/dev/null 2>&1; then
  GITBIN=$(command -v git)
  printf 'expose build_queue off\ntool smoke_git %s read 10 65536 off %s -- --no-pager status --short --branch\n' "$GITBIN" "$PWD" > "$TMP/good.conf"
  EXT=$(SBM_TOOLS_CONF="$TMP/good.conf" "$BIN" --list-tools 2>/dev/null |
    python3 -c "import json,sys; print('\n'.join(t['name'] for t in json.load(sys.stdin)['tools']))")
  printf '%s\n' "$EXT" | grep -qx smoke_git || fail "external not listed"
  printf '%s\n' "$EXT" | grep -qx build_queue && fail "expose off ignored"
  [ "$(printf '%s\n' "$EXT" | wc -l | tr -d ' ')" = "23" ] || fail "expose+external count"
  printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"smoke_git","arguments":{}}}' |
    SBM_TOOLS_CONF="$TMP/good.conf" "$BIN" --mcp 2>/dev/null |
    grep -q '"isError":false' || fail "external git call failed"
  pass "expose-off + git external roundtrip"
else
  echo "SMOKE-SKIP: no git for external test"
fi

# 4. errors and ping (docs/mcp-protocol.md, docs/jsonrpc-examples.md)
printf '%s\n' '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"no_such_tool","arguments":{}}}' |
  "$BIN" --mcp 2>/dev/null | grep -q '"code":-32602' || fail "unknown tool code"
printf '%s\n' '{"jsonrpc":"2.0","id":10,"method":"resources/list"}' |
  "$BIN" --mcp 2>/dev/null | grep -q '"code":-32601' || fail "resources/list code"
printf 'hello\n' | "$BIN" --mcp 2>/dev/null | grep -q '"code":-32700' || fail "garbage code"
printf '{"jsonrpc":"2.0","id":8,"method":"ping"}\r\n' |
  "$BIN" --mcp 2>/dev/null | grep -q '"id":8,"result":{}' || fail "ping/crlf"
pass "errors -32602/-32601/-32700, ping, CRLF"

# 5. Content-Length framing mirrors (docs/jsonrpc-examples.md)
python3 -c "
import json,subprocess,os
body=json.dumps({'jsonrpc':'2.0','id':7,'method':'initialize','params':{'protocolVersion':'2025-06-18'}})
p=subprocess.run(['./saphira-build-mcp','--mcp'],input=('Content-Length: %d\r\n\r\n'%len(body)+body),
  capture_output=True,text=True,env=dict(os.environ,SBM_NOTES_DB='$TMP/notes.db',SBM_TOOLS_CONF='/nonexistent-tools.conf'),timeout=30)
assert p.stdout.startswith('Content-Length: '), p.stdout[:80]
print('lsp-ok')
" | grep -q lsp-ok || fail "content-length framing"
pass "content-length framing"

# 6. journal roundtrip + recall (docs/recovery.md)
SAVE=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_save","arguments":{"label":"smoke","body":"smoke probe"}}}' |
  "$BIN" --mcp 2>/dev/null)
printf '%s' "$SAVE" | grep -q "recorded checkpoint" || fail "checkpoint_save"
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"checkpoint_resume","arguments":{"label":"smoke"}}}' |
  "$BIN" --mcp 2>/dev/null | grep -q "smoke probe" || fail "checkpoint_resume"
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"total_recall","arguments":{"mode":"query","query":"smoke","budget":"compact","limit":2,"detail":"summary"}}}' |
  "$BIN" --mcp 2>/dev/null | grep -q "not searched: tool outputs" || fail "total_recall"
ID=$(printf '%s' "$SAVE" | python3 -c "import json,sys,re; print(re.search(r'#?(\d+)', json.load(sys.stdin)['result']['content'][0]['text']).group(1))")
printf '%s\n' "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"checkpoint_close\",\"arguments\":{\"id\":$ID}}}" |
  "$BIN" --mcp 2>/dev/null | grep -q "closed checkpoint" || fail "checkpoint_close"
pass "checkpoint save/resume/recall/close roundtrip"

# 7. python stdio client (docs/examples/mcp-stdio-client.py): output is
# two JSON documents (init object, then the name array); parse both.
python3 docs/examples/mcp-stdio-client.py --exe "$BIN" list 2>"$TMP/client.err" >"$TMP/client.out"
N=$(python3 -c "import json,sys; d=json.JSONDecoder(); s=open('$TMP/client.out').read(); _,i=d.raw_decode(s); print(len(d.raw_decode(s[i:].strip())[0]))" 2>"$TMP/client.parse-err" || echo PARSE-FAIL)
[ "$N" = "23" ] || { echo "--- client.out head:"; head -c 400 "$TMP/client.out"; echo; echo "--- client.err:"; head -c 400 "$TMP/client.err"; echo; echo "--- parse-err:"; head -c 300 "$TMP/client.parse-err"; fail "python client list (got $N)"; }
pass "python stdio client"

# 8. http bridge + curl (docs/http-and-curl.md)
if command -v curl >/dev/null 2>&1; then
  SBM_EXE="$PWD/$BIN" python3 docs/examples/stdio-http-bridge.py 8479 \
    >/dev/null 2>&1 &
  BRIDGEPID=$!
  sleep 1
  [ "$(curl -s localhost:8479/health | python3 -c "import json,sys; print(json.load(sys.stdin)['ok'])" 2>/dev/null)" = "True" ] || fail "bridge health"
  [ "$(curl -s localhost:8479/list | python3 -c "import json,sys; print(len(json.load(sys.stdin)['tools']))" 2>/dev/null)" = "23" ] || fail "bridge list"
  curl -s localhost:8479/call -d '{"tool":"checkpoint_list","arguments":{"limit":1}}' |
    grep -q '"ok": true' || fail "bridge call"
  curl -s localhost:8479/call -d '{"tool":"no_such_tool","arguments":{}}' |
    grep -q '"code": -32602' || fail "bridge error passthrough"
  kill $BRIDGEPID 2>/dev/null
  BRIDGEPID=""
  pass "bridge + curl health/list/call/errors"
else
  echo "SMOKE-SKIP: no curl for bridge test"
fi

# 9. docs structural coverage: every built-in documented with curl + powershell
for t in $(names ''); do
  grep -q "\`$t\`" docs/tools.md || fail "tools.md missing $t"
done
[ "$(grep -c 'localhost:8473/call' docs/tools.md)" -ge 23 ] || fail "tools.md curl coverage"
[ "$(grep -c 'mcp-stdio-client.ps1' docs/tools.md)" -ge 23 ] || fail "tools.md powershell coverage"
for f in docs/mcp-protocol.md docs/tools.md docs/jsonrpc-examples.md docs/http-and-curl.md \
  docs/recovery.md docs/windows.md docs/external-tools.md docs/ssh-remote.md \
  docs/examples/mcp-stdio-client.py docs/examples/mcp-stdio-client.ps1 \
  docs/examples/stdio-http-bridge.py; do
  [ -f "$f" ] || fail "missing $f"
done
pass "docs structural coverage (23 tools x curl x powershell)"

echo "DOCS-SMOKE: ALL GREEN"
