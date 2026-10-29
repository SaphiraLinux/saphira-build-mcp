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

static void conf_fail(ext_conf *c, const char *why) {
	size_t n;
	c->ok = 0;
	c->ntools = 0;
	c->noff = 0;
	n = strlen(why);
	if (n > sizeof c->err - 1)
		n = sizeof c->err - 1;
	memcpy(c->err, why, n);
	c->err[n] = 0;
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
			fclose(f);
			conf_fail(c, "line too long");
			return;
		}
		/* Comment: # at start or after whitespace ends the line. */
		hash = strchr(line, '#');
		if (hash && (hash == line || hash[-1] == ' ' ||
		    hash[-1] == '\t' || hash[-1] == '\n'))
			*hash = 0;
		n = split(line, toks, (int)(sizeof toks / sizeof toks[0]));
		if (n < 0) {
			fclose(f);
			conf_fail(c, "too many tokens");
			return;
		}
		if (n == 0)
			continue;
		if (strcmp(toks[0], "expose") == 0) {
			if (n != 3 || (strcmp(toks[2], "on") != 0 &&
			    strcmp(toks[2], "off") != 0)) {
				fclose(f);
				conf_fail(c, "bad expose line");
				return;
			}
			if (has_metachars(toks[1])) {
				fclose(f);
				conf_fail(c, "bad expose name");
				return;
			}
			if (strcmp(toks[2], "off") == 0) {
				if (c->noff >= (int)(sizeof c->off / sizeof c->off[0])) {
					fclose(f);
					conf_fail(c, "too many expose lines");
					return;
				}
				c->off[c->noff] = strdup(toks[1]);
				if (!c->off[c->noff]) {
					fclose(f);
					conf_fail(c, "out of memory");
					return;
				}
				c->noff++;
			}
			continue;
		}
		if (strcmp(toks[0], "tool") != 0) {
			fclose(f);
			conf_fail(c, "unknown directive");
			return;
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
			fclose(f);
			conf_fail(c, "bad tool line shape");
			return;
		}
		if (!valid_name(toks[1]) || toks[2][0] != '/' ||
		    has_dotdot(toks[2]) ||
		    (strcmp(toks[3], "read") != 0 && strcmp(toks[3], "test") != 0) ||
		    (strcmp(toks[6], "on") != 0 && strcmp(toks[6], "off") != 0) ||
		    toks[7][0] != '/' || has_dotdot(toks[7])) {
			fclose(f);
			conf_fail(c, "bad tool fields");
			return;
		}
		if (denied_exe(toks[2]) || denied_state(toks[2]) ||
		    denied_state(toks[7]) || has_metachars(toks[2]) ||
		    has_metachars(toks[7])) {
			fclose(f);
			conf_fail(c, "tool path refused");
			return;
		}
		{
			char *end = NULL;
			long to = strtol(toks[4], &end, 10);
			long mb;
			if (!end || *end || to < 1 || to > 3600) {
				fclose(f);
				conf_fail(c, "bad timeout");
				return;
			}
			end = NULL;
			mb = strtol(toks[5], &end, 10);
			if (!end || *end || mb < 1 || mb > 1048576) {
				fclose(f);
				conf_fail(c, "bad max-bytes");
				return;
			}
			if (c->ntools >= CONF_MAX_TOOLS) {
				fclose(f);
				conf_fail(c, "too many tools");
				return;
			}
			if (n - dash - 1 > CONF_MAX_ARGV - 1) {
				fclose(f);
				conf_fail(c, "too many args");
				return;
			}
			for (i = dash + 1; i < n; i++) {
				if (has_metachars(toks[i]) ||
				    denied_state(toks[i])) {
					fclose(f);
					conf_fail(c, "tool arg refused");
					return;
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
			t->argv = calloc((size_t)(n - dash), sizeof(char *));
			if (!t->name || !t->exe || !t->cls || !t->dir || !t->argv) {
				free(t->name);
				free(t->exe);
				free(t->cls);
				free(t->dir);
				free(t->argv);
				fclose(f);
				conf_fail(c, "out of memory");
				return;
			}
			t->argv[0] = strdup(toks[2]);
			if (!t->argv[0]) {
				free(t->name);
				free(t->exe);
				free(t->cls);
				free(t->dir);
				free(t->argv);
				fclose(f);
				conf_fail(c, "out of memory");
				return;
			}
			t->argc = 1;
			for (i = dash + 1; i < n; i++) {
				t->argv[t->argc] = strdup(toks[i]);
				if (!t->argv[t->argc]) {
					fclose(f);
					conf_fail(c, "out of memory");
					return;
				}
				t->argc++;
			}
			t->argv[t->argc] = NULL;
			for (i = 0; i < c->ntools; i++) {
				if (strcmp(c->tools[i].name, t->name) == 0) {
					fclose(f);
					conf_fail(c, "duplicate tool name");
					return;
				}
			}
			c->ntools++;
		}
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
