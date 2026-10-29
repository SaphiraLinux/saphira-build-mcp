#!/bin/sh
# install.sh: install the prebuilt saphira-build-mcp binary matching
# this host, and optionally wire it into the agents installed here.
#
#   ./install.sh                                   # binary + man page only
#   ./install.sh --shared | --builtin              # force SQLite linkage
#   ./install.sh --configure-agents                # plus detected agents
#   ./install.sh --configure-agents --with-rules=FILE
#                                                  # plus append checkpoint
#                                                  # rules to FILE
#   PREFIX=/opt/sb ./install.sh ...                # explicit prefix wins
#   DESTDIR=/tmp/stage ./install.sh ...            # staged installs
#
# Variant: each platform ships small (shared system SQLite) and
# large (-builtin, SQLite compiled in). Default is automatic:
# shared where the system provides SQLite, builtin elsewhere.
# --shared / --builtin force either (the proof run below still
# refuses a binary that cannot run here).
#
# Install location mirrors `make install`: root installs system-wide
# (/usr/bin), an ordinary user installs into ~/.local/bin.
#
# Safeguards: every configuration write is backup-first
# (.bak-sbm-<date> beside the original); an entry that already
# exists is left alone and reported; anything unrecognized falls
# back to printed manual instructions instead of a guess.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date +%Y%m%d)-$$

if [ -n "${PREFIX-}" ]; then
	PREFIX=$PREFIX
elif [ "$(id -u 2>/dev/null || echo 1)" = "0" ]; then
	PREFIX=/usr
else
	PREFIX=${HOME:?HOME unset}/.local
fi
DESTDIR=${DESTDIR-}
BINPATH=$DESTDIR$PREFIX/bin/saphira-build-mcp

CONFIGURE=0
RULES_FILE=""
VARIANT=auto
for a in "$@"; do
	case "$a" in
	--configure-agents) CONFIGURE=1 ;;
	--shared) VARIANT=shared ;;
	--builtin) VARIANT=builtin ;;
	--with-rules=*) RULES_FILE=${a#--with-rules=} ;;
	--with-rules)
		echo "install.sh: --with-rules needs FILE (use --with-rules=FILE)" >&2
		exit 2
		;;
	-*) echo "install.sh: unknown flag $a" >&2; exit 2 ;;
	*) echo "install.sh: unexpected argument $a" >&2; exit 2 ;;
	esac
done

# System SQLite present? Decides shared vs builtin on auto.
# Darwin carries libsqlite3.dylib in the base system; elsewhere
# look in the loader's usual paths.
system_sqlite() {
	case "$(uname -s)" in
	Darwin) test -f /usr/lib/libsqlite3.dylib ;;
	*) (ldconfig -p 2>/dev/null | grep -q libsqlite3) || \
		ls /lib/libsqlite3.so* /usr/lib/libsqlite3.so* \
		   /usr/lib/*/libsqlite3.so* 2>/dev/null | grep -q . ;;
	esac
}

OS=$(uname -s)
if [ "$OS" = "Darwin" ]; then
	FAM=saphira-build-mcp.osx
elif command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | grep -qi musl; then
	FAM=saphira-build-mcp.musl
else
	FAM=saphira-build-mcp.arch
fi
# Never pick a Linux binary for macOS merely because the
# architecture matches: Darwin is decided first, above.
case "$VARIANT" in
shared) BIN=$FAM ;;
builtin) BIN=$FAM-builtin ;;
*) if system_sqlite; then BIN=$FAM; else BIN=$FAM-builtin; fi ;;
esac

if [ ! -f "$HERE/$BIN" ]; then
	echo "install.sh: no binary for this host (wanted $BIN)" >&2
	exit 1
fi

# Checksum gate: sha256sum on Linux, shasum on macOS. No hasher,
# no install.
if command -v sha256sum >/dev/null 2>&1; then
	SUM="sha256sum $HERE/$BIN"
elif command -v shasum >/dev/null 2>&1; then
	SUM="shasum -a 256 $HERE/$BIN"
else
	echo "install.sh: need sha256sum or shasum to verify $BIN" >&2
	exit 1
fi
want=$(grep " $BIN\$" "$HERE/SHA256SUMS" 2>/dev/null | cut -d' ' -f1 || true)
got=$($SUM | cut -d' ' -f1)
if [ -z "$want" ] || [ "$want" != "$got" ]; then
	echo "install.sh: checksum mismatch for $BIN (aborting)" >&2
	exit 1
fi

install -d "$DESTDIR$PREFIX/bin"
install -m 0755 "$HERE/$BIN" "$BINPATH"
install -d "$DESTDIR$PREFIX/share/man/man1"
install -m 0644 "$HERE/saphira-build-mcp.1" \
	"$DESTDIR$PREFIX/share/man/man1/saphira-build-mcp.1"

# Proof the installed copy runs: tools/list must exit 0.
if "$BINPATH" --list-tools >/dev/null 2>&1; then
	echo "installed $BINPATH ($BIN, verified)"
else
	echo "install.sh: installed binary failed to run" >&2
	case "$BIN" in
	*-builtin) ;;
	*) echo "install.sh: this host may lack system SQLite; retry with --builtin" >&2 ;;
	esac
	exit 1
fi

[ "$CONFIGURE" = "1" ] || exit 0

# --- agent configuration (opt-in only) -------------------------------
JAUTHOR=$(id -un 2>/dev/null || echo user)
JOURNAL=${HOME:?HOME unset}/.local/share/saphira-build-mcp/notes.db
OPCODE_JSONC=$HOME/.config/opencode/opencode.jsonc

opencode_entry() {
	cat <<EOF
    // saphira-build-mcp in portable mode (installed $(date +%Y-%m-%d)).
    "saphira-build": {
      "type": "local",
      "command": [
        "$BINPATH",
        "--mcp"
      ],
      "environment": {
        "SBM_SAPHIRA": "off",
        "SBM_AUTHOR": "$JAUTHOR",
        "SBM_NOTES_DB": "$JOURNAL"
      },
      "enabled": true
    }
EOF
}

opencode_entry_comma() {
	opencode_entry | sed '$s/    }$/    },/'
}

opencode_manual() {
	cat <<EOF
  add a "saphira-build" object under the top-level "mcp" key:
    "saphira-build": {
      "type": "local",
      "command": ["$BINPATH", "--mcp"],
      "environment": {
        "SBM_SAPHIRA": "off",
        "SBM_AUTHOR": "$JAUTHOR",
        "SBM_NOTES_DB": "$JOURNAL"
      },
      "enabled": true
    }
EOF
}

claude_manual() {
	cat <<EOF
  per Anthropic's docs, from the project directory:
    claude mcp add saphira-build --transport stdio \\
      --env SBM_SAPHIRA=off --env SBM_AUTHOR=$JAUTHOR \\
      --env SBM_NOTES_DB=$JOURNAL -- $BINPATH --mcp
  then confirm with: claude mcp list
  (invocation shape per vendor docs; verify locally before trusting it)
EOF
}

opencode_validate() {
	# $1 = edited file. Re-parse comment-stripped JSON when
	# python3 exists; on failure restore the backup and fall
	# back to manual instructions. No python3: the insertion
	# is brace-matched by construction, reported unverified.
	if ! command -v python3 >/dev/null 2>&1; then
		echo "opencode: no python3 here; entry written but unvalidated (backup kept)"
		return 0
	fi
	if python3 -c 'import json,re,sys; s=open(sys.argv[1]).read(); d=json.loads(re.sub(r"(?m)^\s*//.*$","",s)); d["mcp"]["saphira-build"]["command"]; print("ok")' "$1" 2>/dev/null | grep -q ok; then
		echo "opencode: entry validated"
	else
		mv "$1.bak-sbm-$STAMP" "$1"
		echo "opencode: entry failed validation; backup restored, add manually:"
		opencode_manual
	fi
}

# opencode: extend an existing top-level "mcp" object, backup first.
# Anything else (no file, no mcp block, entry present, odd shape)
# reports manual instructions instead of guessing.
if [ -f "$OPCODE_JSONC" ]; then
	if grep -q '"saphira-build"' "$OPCODE_JSONC" 2>/dev/null; then
		echo "opencode: saphira-build entry present, left alone"
	else
		mline=$(grep -n '^  "mcp": {$' "$OPCODE_JSONC" | head -1 | cut -d: -f1 || true)
		if [ -n "$mline" ]; then
			next=$(sed -n "$((mline + 1))p" "$OPCODE_JSONC")
			case "$next" in
			'  }'|'  },')
				# empty block: entry becomes the only child.
				cp "$OPCODE_JSONC" "$OPCODE_JSONC.bak-sbm-$STAMP"
				{ sed -n "1,${mline}p" "$OPCODE_JSONC"; opencode_entry; echo "$next"; sed -n "$((mline + 2)),\$p" "$OPCODE_JSONC"; } > "$OPCODE_JSONC.new"
				mv "$OPCODE_JSONC.new" "$OPCODE_JSONC"
				echo "opencode: entry added (backup $OPCODE_JSONC.bak-sbm-$STAMP)"
				opencode_validate "$OPCODE_JSONC"
				;;
			*)
				# non-empty block: insert first, comma-terminated.
				cp "$OPCODE_JSONC" "$OPCODE_JSONC.bak-sbm-$STAMP"
				{ sed -n "1,${mline}p" "$OPCODE_JSONC"; opencode_entry_comma; sed -n "$((mline + 1)),\$p" "$OPCODE_JSONC"; } > "$OPCODE_JSONC.new"
				mv "$OPCODE_JSONC.new" "$OPCODE_JSONC"
				echo "opencode: entry added (backup $OPCODE_JSONC.bak-sbm-$STAMP)"
				opencode_validate "$OPCODE_JSONC"
				;;
			esac
		else
			echo "opencode: no top-level mcp block found; add manually:"
			opencode_manual
		fi
	fi
else
	echo "opencode: no config at $OPCODE_JSONC; add manually:"
	opencode_manual
fi

# codex: vendor CLI (verified invocation), skip when present.
if command -v codex >/dev/null 2>&1; then
	if codex mcp get saphira-build >/dev/null 2>&1; then
		echo "codex: saphira-build entry present, left alone"
	else
		codex mcp add saphira-build \
			--env SBM_SAPHIRA=off \
			--env SBM_AUTHOR="$JAUTHOR" \
			--env SBM_NOTES_DB="$JOURNAL" \
			-- "$BINPATH" --mcp \
		&& echo "codex: entry added" \
		|| echo "codex: add failed; add manually: codex mcp add saphira-build --env SBM_SAPHIRA=off -- \"$BINPATH\" --mcp"
	fi
fi

# claude: no verified CLI invocation on our hosts; print, don't guess.
if command -v claude >/dev/null 2>&1; then
	echo "claude: detected; configure with the documented command:"
	claude_manual
fi

if [ -n "$RULES_FILE" ]; then
	if grep -q "Agent checkpoint rules (saphira-build-mcp)" "$RULES_FILE" 2>/dev/null; then
		echo "rules: checkpoint discipline present in $RULES_FILE, left alone"
	else
		cp "$RULES_FILE" "$RULES_FILE.bak-sbm-$STAMP" 2>/dev/null || true
		{
			echo ""
			cat "$HERE/rules/AGENT_MCP_RULES.md"
		} >> "$RULES_FILE"
		echo "rules: template appended to $RULES_FILE"
	fi
else
	echo "rules: pass --with-rules=FILE to append the checkpoint discipline"
	echo "  (template: $HERE/rules/AGENT_MCP_RULES.md), or ask your agent:"
	echo "  'Read rules/AGENT_MCP_RULES.md and install its checkpoint"
	echo "   discipline into my persistent instructions.'"
fi

echo "done. Verify: save a checkpoint, restart the agent, confirm the"
echo "fresh session lists the register before doing anything else."
