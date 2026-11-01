#ifndef SBM_CONF_H
#define SBM_CONF_H

#include "core.h"

/* /etc/saphira/mcp/tools.conf: extension registry plus exposure
 * allowlist. Strict line grammar, fail-closed per line: a malformed
 * or unsafe line disables only that line (skipped, counted, first
 * error kept); unrelated entries keep working. No shell anywhere:
 * registered tools run with fixed argv via execve (see runner.c).
 *
 *   expose <builtin-name> <on|off>
 *   lockdown_ssh[= ]<true|false>   (default true; last valid wins)
 *   tool <name> <abspath> <class> <timeout-s> <max-bytes> <sandbox:on|off> <dir> -- <fixed args...>
 *
 * class is read, test or ask. read runs unsandboxed, test runs
 * sandboxed; ask behaves like read for execution but additionally
 * accepts one caller string (the MCP `prompt` argument), delivered
 * on a dedicated stdin pipe closed with EOF after writing. No other
 * caller input exists: the command, flags and environment stay
 * operator-fixed. <dir> is the child cwd, absolute, without "..".
 * Everything after -- is fixed argv.
 *
 * lockdown_ssh is operator-owned SSH policy, default true (refuse):
 * while true, any tool naming an ssh executable is skipped like any
 * other refused line. When the operator sets it false, explicitly
 * configured ssh executables may run through the same fixed-argv,
 * timeout, audit and sandbox interface. Nothing else about the
 * denylist changes: shells, other network tools, the
 * build/sign/publish/install lane and MCP-owned state stay
 * unregistrable either way. */

#define CONF_MAX_TOOLS 64
#define CONF_MAX_ARGV 65 /* argv[0]..argv[63] plus NULL */
#define CONF_MAX_LINE 4096

typedef struct {
	char *name;   /* [a-z][a-z0-9_]{0,31}, no builtin collision */
	char *exe;    /* abspath of a denylisted-checked executable */
	char *cls;    /* "read", "test" or "ask" */
	char **argv;  /* fixed argv, argv[0] is exe, NULL-terminated */
	int argc;     /* includes argv[0], >= 1 */
	char *dir;    /* child cwd */
	long timeout_s;  /* 1..3600 */
	long max_bytes;  /* 1..1048576 */
	int sandbox;  /* test class defaults on, read/ask default off */
} ext_tool;

typedef struct {
	char *off[32];
	int noff;
	ext_tool tools[CONF_MAX_TOOLS];
	int ntools;
	int ok;         /* extensions usable (or conf absent: none) */
	int present;    /* conf file was found and readable */
	int lockdown_ssh; /* 1 = refuse ssh executables (default) */
	int nskipped;   /* refused lines skipped without harming the rest */
	char err[256];  /* first skip reason, with line number */
} ext_conf;

const char *path_tools_conf(void);

/* Portable mode (SBM_SAPHIRA): 1 is builder mode (unset or "on"),
 * 0 is portable ("off": Saphira-state tools gated away, journal
 * and checkpoints stay on), -1 is an invalid value. main() refuses
 * -1 before serving anything, so the library paths below only ever
 * see 1 or 0; where -1 could theoretically arrive it fails closed
 * toward gated (never silently back to on). */
int saphira_mode(void);

/* Build-execution capability (SBM_BUILD): 1 means privileged build
 * operations are exposed (unset or "on"), 0 gates build_request and
 * package_bump away while journal, policy, checkpoint and recovery
 * tools stay fully on, -1 is invalid (main() refuses it the same
 * way). The two capabilities are independent: a host can serve
 * builder state without build permission, or carry no builder
 * state at all. */
int build_mode(void);

void conf_load(ext_conf *c);
void conf_free(ext_conf *c);
const ext_tool *conf_find(const ext_conf *c, const char *name);

#endif
