#!/bin/sh
# packaging/freebsd/make-package.sh: build a native FreeBSD pkg from source.
#
# Not a ports entry and not Saphira buildpkg: this drives pkg(8) directly
# with a manifest+plist so the on-disk result matches what a ports tree
# would produce (PREFIX=/usr/local, man page 1.gz, LICENSE registered,
# runtime dependency on the shared libsqlite3 declared and version-pinned
# to the host that built the package).
#
# Usage:  sh packaging/freebsd/make-package.sh
# Env:    MAINTAINER=<email>   override the +MANIFEST maintainer field
#
# Output: packaging/freebsd/dist/<name>-<version>.pkg (+ .SHA256)
# Verify: pkg info -F <pkgfile>; then install as root with
#         sudo pkg add <pkgfile>  (resolves sqlite3 automatically).

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

MAKE=${MAKE:-gmake}
OUT=${OUT:-$root/packaging/freebsd/dist}
mkdir -p "$OUT"
WORK=$(mktemp -d "$OUT/.work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

for tool in "$MAKE" pkg gzip tar; do
	command -v "$tool" >/dev/null 2>&1 || {
		printf 'make-package: required tool not found: %s\n' "$tool" >&2
		exit 1
	}
done

# --- build (dynamic: the package declares libsqlite3 as a dependency) --
"$MAKE" clean
"$MAKE"

BIN="$root/saphira-build-mcp"
[ -x "$BIN" ] || { printf 'make-package: build failed\n' >&2; exit 1; }

VERSION=$("$BIN" --version | awk '{print $2}')
[ -n "$VERSION" ] || { printf 'make-package: version undetectable\n' >&2; exit 1; }

# --- dependency: exact installed sqlite3, so pkg resolves at install ----
SQLITE_VERSION=$(pkg info sqlite3 | awk '/^Version[[:space:]]*:/ {print $3}')
[ -n "$SQLITE_VERSION" ] || {
	printf 'make-package: cannot read installed sqlite3 version (pkg info)\n' >&2
	exit 1
}

MAINTAINER=${MAINTAINER:-$(git config user.email || true)}
[ -n "$MAINTAINER" ] || MAINTAINER=<maintainer@example.invalid>: set MAINTAINER

# --- stage with the prefix the package will install under --------------
PREFIX=/usr/local
"$MAKE" install DESTDIR="$WORK/stage" PREFIX=$PREFIX
gzip -n -9 "$WORK/stage$PREFIX/share/man/man1/saphira-build-mcp.1"

# --- manifest ----------------------------------------------------------
cat > "$WORK/+MANIFEST" <<EOF
{
  "name": "saphira-build-mcp",
  "origin": "devel/saphira-build-mcp",
  "version": "$VERSION",
  "comment": "Agent recovery journal and Saphira build-query MCP server",
  "desc": "A local Model Context Protocol (MCP) server over stdio with two capabilities on one binary: portable agent recovery (checkpoints, session handoff, notes journal, total recall) and optional Saphira build integration behind SBM_SAPHIRA=on. It never builds, signs, publishes, installs or restarts anything, and it never executes recipe scripts; missing data sources produce explicit errors, never invented results.",
  "maintainer": "$MAINTAINER",
  "www": "https://github.com/SaphiraLinux/saphira-build-mcp",
  "categories": ["devel"],
  "arch": "amd64",
  "licenses": ["BUSL-1.1"],
  "prefix": "$PREFIX",
  "deps": {
    "sqlite3": {
      "origin": "databases/sqlite3",
      "version": "$SQLITE_VERSION"
    }
  }
}
EOF

# --- plist (paths are relative to "prefix"; directives per entry) ------
cat > "$WORK/plist" <<EOF
@owner root
@group wheel
@mode 0755
bin/saphira-build-mcp
@mode 0644
share/man/man1/saphira-build-mcp.1.gz
@owner root
@group wheel
@mode 0444
share/licenses/saphira-build-mcp/LICENSE
EOF

install -d "$WORK/stage$PREFIX/share/licenses/saphira-build-mcp"
install -m 0444 "$root/LICENSE" "$WORK/stage$PREFIX/share/licenses/saphira-build-mcp/LICENSE"

pkg create --quiet --out-dir "$OUT" --root-dir "$WORK/stage" \
	--plist "$WORK/plist" --manifest "$WORK/+MANIFEST"

PKGFILE=$(ls "$OUT"/saphira-build-mcp-*.pkg 2>/dev/null | head -1)
[ -n "$PKGFILE" ] || { printf 'make-package: pkg create produced no file\n' >&2; exit 1; }

# --- verification (unprivileged) ---------------------------------------
pkg info -F "$PKGFILE" || exit 1
printf '\n'
XDIR="$WORK/x"
mkdir -p "$XDIR"
tar -xf "$PKGFILE" -C "$XDIR"
[ -x "$XDIR$PREFIX/bin/saphira-build-mcp" ] || {
	printf 'make-package: extracted package lacks the binary\n' >&2
	exit 1
}
cmp -s "$BIN" "$XDIR$PREFIX/bin/saphira-build-mcp" || {
	printf 'make-package: packaged binary differs from the build\n' >&2
	exit 1
}
[ -s "$XDIR$PREFIX/share/man/man1/saphira-build-mcp.1.gz" ] ||
	{ printf 'make-package: man page missing in package\n' >&2; exit 1; }
"$XDIR$PREFIX/bin/saphira-build-mcp" --help >/dev/null 2>&1 || {
	printf 'make-package: packaged binary does not run\n' >&2
	exit 1
}

# Sidecar digest in the coreutils/sha256sum format ("digest  filename"),
# filename relative to the sidecar so `sha256sum -c` verifies in place.
# The previous line assumed coreutils output ("digest file") and swapped
# fields, but it called the base `sha256`, which prints
# "SHA256 (file) = digest" - the swap emitted "(file)  SHA256" with no
# digest at all. Also note base `sha256 -c` fails to catch mismatched
# digests (verified 15.1: mismatched, missing and malformed lines all
# exit 0 silently) - never verify with it; use `sha256sum -c`, or on
# bases without sha256sum (13/14) compare `sha256 -q` against the
# sidecar's first field.
PKG_BASE=$(basename "$PKGFILE")
if command -v sha256sum >/dev/null 2>&1; then
	(cd "$OUT" && sha256sum "$PKG_BASE") > "$PKGFILE.SHA256"
else
	printf '%s  %s\n' "$(sha256 -q "$PKGFILE")" "$PKG_BASE" > "$PKGFILE.SHA256"
fi

# Prove the sidecar verifies before shipping it.
(cd "$OUT" && sha256sum -c "$PKG_BASE.SHA256" >/dev/null) ||
	{ printf 'make-package: sidecar does not verify\n' >&2; exit 1; }

printf 'make-package: OK\n';
ls -l "$PKGFILE" "$PKGFILE.SHA256"
