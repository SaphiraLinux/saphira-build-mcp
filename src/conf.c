#include "conf.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Executables that can never be registered. Three groups:
 * shells/interpreters (arbitrary code), command-executing filters
 * (awk/find/xargs run subcommands, so a path allowlist alone is not
 * sufficient), and anything that routes around MCP-owned build and
 * package operations (the runner must never become a second build,
 * sign, publish or install path). */
static const char *deny_exe[] = {
	"sh", "bash", "dash", "ash", "zsh", "ksh", "fish", "tcsh", "csh",
	"python", "python2", "python3", "perl", "ruby", "lua", "luajit",
	"php", "node", "nodejs", "deno",
	"env", "su", "sudo", "doas",
	"apk", "buildpkg", "buildpkg-single", "bumppkg", "makepkg",
	"resolvepkg", "sign-apk-repo", "installpkg",
	"awk", "gawk", "mawk", "nawk", "busybox", "find", "xargs",
	"parallel", "script", "setsid",
	"chroot", "nsenter", "unshare", "bwrap",
	"git-shell", "git-receive-pack", "git-upload-pack",
	"ssh", "sshd", "scp", "curl", "wget", "ftp", "nc", "ncat",
	"socat", "telnet",
	"ed", "ex", "vi", "vim", "nano", "emacs",
	"tee", "dd", "install", "cp", "mv", "ln", "rm", "mkdir",
	"rmdir", "touch", "chmod", "chown", "mknod",
	"cc", "gcc", "clang", "tcc", "ccache", "make", "cmake",
	"ninja", "meson", "go", "rustc", "cargo",
	"timeout", "watch",
	NULL
};

/* Substrings that must never appear in an exe, dir or fixed arg: the
 * MCP-owned writable state the runner must not route around. */
static const char *deny_state[] = {
	"queue.db", "repository.db", "notes.db", "stage4/incoming",
	"/out/", ".buildpkg",
	NULL
};

const char *path_tools_conf(void) {
	return env_or("SBM_TOOLS_CONF", "/etc/saphira/mcp/tools.conf");
}

int saphira_mode(void) {
	const char *v = getenv("SBM_SAPHIRA");
	if (!v || strcmp(v, "on") == 0)
		return 1;
	if (strcmp(v, "off") == 0)
		return 0;
	return -1;
}

int build_mode(void) {
	const char *v = getenv("SBM_BUILD");
	if (!v || strcmp(v, "on") == 0)
		return 1;
	if (strcmp(v, "off") == 0)
		return 0;
	return -1;
}

/* One rejected line: skipped without harming the rest. The first
 * reason is kept for diagnostics; the count says how many lines
 * fell out. Extensions parsed from good lines keep working. */
static void conf_skip(ext_conf *c, int lineno, const char *why) {
	c->nskipped++;
	if (!c->err[0])
		snprintf(c->err, sizeof c->err, "line %d: %.220s", lineno, why);
}

/* Pre-scan for the operator's SSH policy before the main parse, so
 * a lockdown_ssh line applies no matter where it sits in the file.
 * The last valid setting wins; absent or invalid means lockdown on
 * (refuse). Fail-closed toward refusal, never toward permission. */
static int scan_lockdown(const char *path) {
	FILE *f;
	char line[CONF_MAX_LINE];
	char val[16];
	int lockdown = 1;
	f = fopen(path, "r");
	if (!f)
		return 1;
	while (fgets(line, sizeof line, f)) {
		char *p = line, *v, *hash;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == 0)
			continue;
		hash = strchr(p, '#');
		if (hash && (hash == p || hash[-1] == ' ' || hash[-1] == '\t'))
			*hash = 0;
		if (strncmp(p, "lockdown_ssh", 12) != 0)
			continue;
		/* Both `lockdown_ssh=false` and `lockdown_ssh false`
		 * spellings accepted; first whitespace-delimited token
		 * after the keyword (or `=`) is the value. */
		if (p[12] == '=')
			v = p + 13;
		else if (p[12] == ' ' || p[12] == '\t') {
			v = p + 12;
			while (*v == ' ' || *v == '\t')
				v++;
		} else {
			continue;
		}
		if (sscanf(v, "%15s", val) == 1) {
			if (strcmp(val, "true") == 0 || strcmp(val, "on") == 0)
				lockdown = 1;
			else if (strcmp(val, "false") == 0 || strcmp(val, "off") == 0)
				lockdown = 0;
			/* Anything else is not a valid setting: keep current. */
		}
	}
	fclose(f);
	return lockdown;
}

void conf_free(ext_conf *c) {
	int i, j;
	for (i = 0; i < c->noff; i++)
		free(c->off[i]);
	for (i = 0; i < c->ntools; i++) {
		free(c->tools[i].name);
		free(c->tools[i].exe);
		free(c->tools[i].cls);
		free(c->tools[i].dir);
		for (j = 0; j < c->tools[i].argc; j++)
			free(c->tools[i].argv[j]);
		free(c->tools[i].argv);
	}
	memset(c, 0, sizeof *c);
}

static int has_metachars(const char *s) {
	for (; *s; s++) {
		switch (*s) {
		case ';': case '|': case '&': case '$': case '"':
		case '\'': case '\\': case '*': case '?': case '~':
		case '(': case ')': case '<': case '>': case '\n':
			return 1;
		}
	}
	return 0;
}

static int has_dotdot(const char *s) {
	size_t i;
	for (i = 0; s[i]; i++) {
		if (s[i] == '.' && s[i + 1] == '.' &&
		    (i == 0 || s[i - 1] == '/') &&
		    (s[i + 2] == 0 || s[i + 2] == '/'))
			return 1;
	}
	return 0;
}

/* Basename is exactly ssh: the one executable lockdown_ssh
 * governs. sshd/scp and every other network tool stay denied
 * unconditionally. */
static int is_ssh_exe(const char *exe) {
	const char *b = strrchr(exe, '/');
	b = b ? b + 1 : exe;
	return strcmp(b, "ssh") == 0;
}

static int denied_exe(const char *exe) {
	const char *b;
	int i;
	b = strrchr(exe, '/');
	b = b ? b + 1 : exe;
	for (i = 0; deny_exe[i]; i++) {
		if (strcmp(b, deny_exe[i]) == 0)
			return 1;
		/* python3.12 style versioned names share the prefix. */
		if ((strcmp(deny_exe[i], "python") == 0 ||
		     strcmp(deny_exe[i], "perl") == 0) &&
		    strncmp(b, deny_exe[i], strlen(deny_exe[i])) == 0 &&
		    (b[strlen(deny_exe[i])] == '.' ||
		     isdigit((unsigned char)b[strlen(deny_exe[i])])))
			return 1;
	}
	return 0;
}

static int denied_state(const char *s) {
	int i;
	for (i = 0; deny_state[i]; i++) {
		if (strstr(s, deny_state[i]))
			return 1;
	}
	return 0;
}

static int valid_name(const char *s) {
	size_t i, n;
	if (!s[0] || !islower((unsigned char)s[0]))
		return 0;
	n = strlen(s);
	if (n > 32)
		return 0;
	for (i = 0; i < n; i++) {
		if (!islower((unsigned char)s[i]) &&
		    !isdigit((unsigned char)s[i]) && s[i] != '_')
			return 0;
	}
	return 1;
}

/* Split line into whitespace-separated tokens (no quoting: a quote
 * char anywhere fails the line at validation). Returns count. */
static int split(char *line, char **toks, int maxt) {
	int n = 0;
	char *p = line;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || *p == '\n')
			break;
		if (n >= maxt)
			return -1;
		toks[n++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		if (*p)
			*p++ = 0;
	}
	return n;
}

void conf_load(ext_conf *c) {
	FILE *f;
	char line[CONF_MAX_LINE];
	char *toks[CONF_MAX_ARGV + 8];
	int lineno = 0;
	const char *path = path_tools_conf();

	memset(c, 0, sizeof *c);
	c->ok = 1;
	/* Operator SSH policy first: the pre-scan finds the last valid
	 * lockdown_ssh setting wherever it sits, defaulting to refuse. */
	c->lockdown_ssh = scan_lockdown(path);
	f = fopen(path, "r");
	if (!f) {
		/* Absent or unreadable: built-ins only, today's
		 * behaviour byte-for-byte. */
		c->present = 0;
		return;
	}
	c->present = 1;
	while (fgets(line, sizeof line, f)) {
		int n, i, dash;
		char *hash;
		ext_tool *t;
		lineno++;
		if (!strchr(line, '\n') && !feof(f)) {
			int ch;
			conf_skip(c, lineno, "line too long");
			/* Drain the remainder so the tail cannot parse
			 * as a new line. */
			while ((ch = fgetc(f)) != '\n' && ch != EOF)
				;
			continue;
		}
		/* Comment: # at start or after whitespace ends the line. */
		hash = strchr(line, '#');
		if (hash && (hash == line || hash[-1] == ' ' ||
		    hash[-1] == '\t' || hash[-1] == '\n'))
			*hash = 0;
		n = split(line, toks, (int)(sizeof toks / sizeof toks[0]));
		if (n < 0) {
			conf_skip(c, lineno, "too many tokens");
			continue;
		}
		if (n == 0)
			continue;
		if (strcmp(toks[0], "expose") == 0) {
			if (n != 3 || (strcmp(toks[2], "on") != 0 &&
			    strcmp(toks[2], "off") != 0)) {
				conf_skip(c, lineno, "bad expose line");
				continue;
			}
			if (has_metachars(toks[1])) {
				conf_skip(c, lineno, "bad expose name");
				continue;
			}
			if (strcmp(toks[2], "off") == 0) {
				if (c->noff >= (int)(sizeof c->off / sizeof c->off[0])) {
					conf_skip(c, lineno, "too many expose lines");
					continue;
				}
				c->off[c->noff] = strdup(toks[1]);
				if (!c->off[c->noff]) {
					conf_skip(c, lineno, "out of memory");
					continue;
				}
				c->noff++;
			}
			continue;
		}
		if (strncmp(toks[0], "lockdown_ssh", 12) == 0) {
			/* Policy already applied by the pre-scan; accept
			 * a well-formed line so a valid setting is not
			 * counted as a skip. Both `lockdown_ssh=false`
			 * and `lockdown_ssh false` spellings accepted. */
			const char *vv = NULL;
			char val[16];
			if (toks[0][12] == '=' && n == 1)
				vv = toks[0] + 13;
			else if (toks[0][12] == 0 && n == 2)
				vv = toks[1];
			if (!vv || sscanf(vv, "%15s", val) != 1 ||
			    (strcmp(val, "true") != 0 &&
			     strcmp(val, "false") != 0 &&
			     strcmp(val, "on") != 0 &&
			     strcmp(val, "off") != 0)) {
				conf_skip(c, lineno, "bad lockdown_ssh line");
				continue;
			}
			continue;
		}
		if (strcmp(toks[0], "tool") != 0) {
			conf_skip(c, lineno, "unknown directive");
			continue;
		}
		/* tool name exe class timeout maxbytes sandbox dir -- args */
		dash = -1;
		for (i = 1; i < n; i++) {
			if (strcmp(toks[i], "--") == 0) {
				dash = i;
				break;
			}
		}
		if (dash != 8) {
			conf_skip(c, lineno, "bad tool line shape");
			continue;
		}
		if (!valid_name(toks[1]) || toks[2][0] != '/' ||
		    has_dotdot(toks[2]) ||
		    (strcmp(toks[3], "read") != 0 && strcmp(toks[3], "test") != 0 &&
		     strcmp(toks[3], "ask") != 0) ||
		    (strcmp(toks[6], "on") != 0 && strcmp(toks[6], "off") != 0) ||
		    toks[7][0] != '/' || has_dotdot(toks[7])) {
			conf_skip(c, lineno, "bad tool fields");
			continue;
		}
		/* ssh is refused while lockdown_ssh holds (the default);
		 * with the operator's explicit false it runs through the
		 * same fixed-argv, timeout, audit and sandbox interface.
		 * Everything else about the denylist is unchanged. */
		if ((denied_exe(toks[2]) &&
		     (c->lockdown_ssh || !is_ssh_exe(toks[2]))) ||
		    denied_state(toks[2]) ||
		    denied_state(toks[7]) || has_metachars(toks[2]) ||
		    has_metachars(toks[7])) {
			conf_skip(c, lineno, "tool path refused");
			continue;
		}
		{
			char *end = NULL;
			long to = strtol(toks[4], &end, 10);
			long mb;
			if (!end || *end || to < 1 || to > 3600) {
				conf_skip(c, lineno, "bad timeout");
				continue;
			}
			end = NULL;
			mb = strtol(toks[5], &end, 10);
			if (!end || *end || mb < 1 || mb > 1048576) {
				conf_skip(c, lineno, "bad max-bytes");
				continue;
			}
			if (c->ntools >= CONF_MAX_TOOLS) {
				conf_skip(c, lineno, "too many tools");
				continue;
			}
			if (n - dash - 1 > CONF_MAX_ARGV - 1) {
				conf_skip(c, lineno, "too many args");
				continue;
			}
			for (i = dash + 1; i < n; i++) {
				if (has_metachars(toks[i]) ||
				    denied_state(toks[i])) {
					conf_skip(c, lineno, "tool arg refused");
					goto next_line;
				}
			}
			t = &c->tools[c->ntools];
			memset(t, 0, sizeof *t);
			t->name = strdup(toks[1]);
			t->exe = strdup(toks[2]);
			t->cls = strdup(toks[3]);
			t->dir = strdup(toks[7]);
		t->timeout_s = to;
		t->max_bytes = mb;
		/* Sandbox defaults on for test, off for read;
		 * conf states it explicitly either way. */
		t->sandbox = strcmp(toks[6], "on") == 0;
		/* argv holds exe + post-"--" args + the NULL
		 * terminator: n - dash + 1 slots. Anything less
		 * writes the terminator past the allocation (heap
		 * corruption that only shows under some allocators
		 * and binary layouts - never rely on the slack). */
		t->argv = calloc((size_t)(n - dash + 1), sizeof(char *));
			if (!t->name || !t->exe || !t->cls || !t->dir || !t->argv) {
				free(t->name);
				free(t->exe);
				free(t->cls);
				free(t->dir);
				free(t->argv);
				conf_skip(c, lineno, "out of memory");
				continue;
			}
			t->argv[0] = strdup(toks[2]);
			if (!t->argv[0]) {
				free(t->name);
				free(t->exe);
				free(t->cls);
				free(t->dir);
				free(t->argv);
				conf_skip(c, lineno, "out of memory");
				continue;
			}
			t->argc = 1;
			for (i = dash + 1; i < n; i++) {
				t->argv[t->argc] = strdup(toks[i]);
				if (!t->argv[t->argc]) {
					conf_skip(c, lineno, "out of memory");
					goto next_line;
				}
				t->argc++;
			}
			t->argv[t->argc] = NULL;
			for (i = 0; i < c->ntools; i++) {
				if (strcmp(c->tools[i].name, t->name) == 0) {
					conf_skip(c, lineno, "duplicate tool name");
					goto next_line;
				}
			}
			c->ntools++;
		}
	next_line: ;
	}
	fclose(f);
}

const ext_tool *conf_find(const ext_conf *c, const char *name) {
	int i;
	if (!c->ok)
		return NULL;
	for (i = 0; i < c->ntools; i++) {
		if (strcmp(c->tools[i].name, name) == 0)
			return &c->tools[i];
	}
	return NULL;
}
