#include "tools.h"
#include "conf.h"
#include "db.h"
#include "runner.h"
#include "sha256.h"
#include "wincompat.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* argument helpers                                                    */
/* ------------------------------------------------------------------ */
static const char *arg_str(jv *args, const char *key) {
	jv *v = args ? jv_get(args, key) : NULL;
	return (v && v->t == 3) ? v->str : NULL;
}

/* Booleans accept real JSON bools and, leniently, "true"/"1" strings:
 * an agent that stringifies its arguments should get the behavior,
 * not a silent default. Returns -1 on wrong-typed values. */
static int arg_bool(jv *args, const char *key, int dflt) {
	jv *v = args ? jv_get(args, key) : NULL;
	if (!v)
		return dflt;
	if (v->t == 1)
		return v->b != 0;
	if (v->t == 3 && v->str) {
		if (strcmp(v->str, "true") == 0 || strcmp(v->str, "1") == 0)
			return 1;
		if (strcmp(v->str, "false") == 0 || strcmp(v->str, "0") == 0)
			return 0;
	}
	return -1;
}

static long arg_long(jv *args, const char *key, long dflt, int *bad) {
	jv *v = args ? jv_get(args, key) : NULL;
	if (!v)
		return dflt;
	if (v->t != 2) {
		*bad = 1;
		return dflt;
	}
	return (long)v->num;
}

static long clamp_long(long v, long lo, long hi) {
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/* ASCII case-insensitive substring. Package metadata here is ASCII;
 * non-ASCII bytes compare by value (deterministic, documented). */
static const char *cistrstr(const char *hay, const char *needle) {
	size_t i, j;
	if (!*needle)
		return hay;
	for (i = 0; hay[i]; i++) {
		for (j = 0; needle[j] &&
		     tolower((unsigned char)hay[i + j]) ==
		     tolower((unsigned char)needle[j]); j++)
			;
		if (!needle[j])
			return hay + i;
		if (!hay[i + j])
			break;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* bounded file reads                                                  */
/* ------------------------------------------------------------------ */
static int read_file_cap(const char *path, size_t cap, gbuf *out,
                         size_t *total, int *truncated) {
	struct stat st;
	FILE *f;
	size_t off = 0;
	*total = 0;
	*truncated = 0;
	if (stat(path, &st) != 0)
		return -1;
	*total = (size_t)st.st_size;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (*total > cap) {
		if (fseek(f, (long)(*total - cap), SEEK_SET) != 0) {
			fclose(f);
			return -1;
		}
		*truncated = 1;
	}
	while (off < cap) {
		char tmp[8192];
		size_t want = cap - off;
		size_t r;
		if (want > sizeof tmp)
			want = sizeof tmp;
		r = fread(tmp, 1, want, f);
		if (r == 0)
			break;
		gbuf_addn(out, tmp, r);
		off += r;
		if (out->failed) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* subprocess: apk only, argv arrays, never a shell                    */
/* ------------------------------------------------------------------ */
#define SPAWN_OUT_CAP (256u * 1024u)
#define SPAWN_ERR_CAP 4096u

/* Run `apk argv...` (execvp, no shell), capturing stdout (capped) and
 * stderr (4K). Returns the exit status, or -1 with err set when the
 * child could not run or be reaped. No timeouts: apk metadata queries
 * are local and fast; a hung apk would hang this call, which the
 * 64K-request / 256K-result caps bound everywhere else. */
static int run_capture(char *const argv[], gbuf *out, gbuf *errput,
                   int *truncated, char *err, size_t errsz) {
#ifdef _WIN32
	/* No fork/exec here: every caller already treats -1 as
	 * "unavailable, fall back or report" (apk fallbacks, empty
	 * commit pins, surfaced guard refusals). */
	(void)argv;
	(void)out;
	(void)errput;
	*truncated = 0;
	snprintf(err, errsz, "subprocess execution is not supported on Windows");
	return -1;
#else
	int po[2], pe[2], st;
	pid_t pid;
	*truncated = 0;
	if (pipe(po) != 0 || pipe(pe) != 0) {
		snprintf(err, errsz, "pipe failed: %s", strerror(errno));
		return -1;
	}
	fcntl(po[0], F_SETFD, FD_CLOEXEC);
	fcntl(po[1], F_SETFD, FD_CLOEXEC);
	fcntl(pe[0], F_SETFD, FD_CLOEXEC);
	fcntl(pe[1], F_SETFD, FD_CLOEXEC);
	pid = fork();
	if (pid < 0) {
		snprintf(err, errsz, "fork failed: %s", strerror(errno));
		close(po[0]);
		close(po[1]);
		close(pe[0]);
		close(pe[1]);
		return -1;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, 0);
			close(devnull);
		}
		dup2(po[1], 1);
		dup2(pe[1], 2);
		close(po[0]);
		close(po[1]);
		close(pe[0]);
		close(pe[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(po[1]);
	close(pe[1]);
	for (;;) {
		char tmp[8192];
		ssize_t r = read(po[0], tmp, sizeof tmp);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		if (out->len < SPAWN_OUT_CAP) {
			size_t room = SPAWN_OUT_CAP - out->len;
			gbuf_addn(out, tmp, (size_t)r < room ? (size_t)r : room);
		} else {
			*truncated = 1;
		}
	}
	close(po[0]);
	for (;;) {
		char tmp[1024];
		ssize_t r = read(pe[0], tmp, sizeof tmp);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		if (errput->len < SPAWN_ERR_CAP)
			gbuf_addn(errput, tmp, (size_t)r);
	}
	close(pe[0]);
	while (waitpid(pid, &st, 0) < 0) {
		if (errno != EINTR) {
			snprintf(err, errsz, "waitpid failed");
			return -1;
		}
	}
	if (out->failed || errput->failed) {
		snprintf(err, errsz, "out of memory capturing apk output");
		return -1;
	}
	if (!WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
#endif
}
/* ------------------------------------------------------------------ */
#define RECIPE_FILE_CAP (128u * 1024u)
#define RECIPE_MAX_PKGS 8192u

typedef struct {
	char name[128];
	char ver[128];
	char rel[16];
	char desc[256];
	char license[64];
	char origin[128];
	char sub[1024];
	char *depends; /* malloc'd, may be NULL */
	int disabled;
} recipe_t;

static void field_copy(char *dst, size_t dstsz, const char *src, size_t n) {
	size_t i = 0, j = 0;
	/* strip one layer of matching outer quotes */
	if (n >= 2 && ((src[0] == '"' && src[n - 1] == '"') ||
	               (src[0] == '\'' && src[n - 1] == '\''))) {
		src++;
		n -= 2;
	}
	while (j + 1 < dstsz && i < n)
		dst[j++] = src[i++];
	dst[j] = 0;
}

/* Extract field `key` from recipe text. Handles single-line values
 * (bare or quoted) and multi-line double-quoted values (depends,
 * subpackages): newlines and indentation collapse to single spaces.
 * Only matches at line starts, so indented function bodies can never
 * contribute fields. Recipes are parsed, never sourced. */
static void recipe_field(const char *text, const char *key,
                         char *out, size_t outsz) {
	size_t kl = strlen(key);
	size_t textlen = strlen(text);
	const char *p = text;
	out[0] = 0;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t llen = nl ? (size_t)(nl - p) : strlen(p);
		if (llen > kl + 1 && memcmp(p, key, kl) == 0 && p[kl] == '=') {
			const char *v = p + kl + 1;
			if (v[0] == '"') {
				const char *q = v + 1;
				const char *end = text + textlen;
				size_t o = 0;
				while (q < end && *q != '"')
					q++;
				if (q >= end)
					return; /* unterminated: leave empty */
				for (const char *c = v + 1; c < q && o + 1 < outsz; c++) {
					if (*c == '\n' || *c == '\t') {
						if (o && out[o - 1] != ' ')
							out[o++] = ' ';
					} else if (*c != '\r') {
						out[o++] = *c;
					}
				}
				out[o] = 0;
				while (o && out[o - 1] == ' ')
					out[--o] = 0;
				return;
			}
			field_copy(out, outsz, v, llen - (kl + 1));
			return;
		}
		if (!nl)
			break;
		p = nl + 1;
	}
}

static int recipe_disabled_value(const char *v) {
	return strcmp(v, "yes") == 0 || strcmp(v, "1") == 0 ||
	       strcmp(v, "true") == 0;
}

/* Collect all recipes. Returns 0 with *out/*n set (caller frees via
 * recipe_free_all), or -1 when the tree cannot even be listed. */
static int recipe_collect(recipe_t **out, size_t *n) {
	DIR *d;
	struct dirent *e;
	recipe_t *arr = NULL;
	size_t nn = 0, cap = 0;
	char tmp[64];
	*out = NULL;
	*n = 0;
	d = opendir(path_recipes());
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char rpath[4096];
		char *text = NULL;
		struct stat st;
		FILE *f;
		size_t got;
		recipe_t *r;
		if (e->d_name[0] == '.')
			continue;
		if (nn >= RECIPE_MAX_PKGS)
			break;
		if (snprintf(rpath, sizeof rpath, "%s/%s/recipe.sh",
		             path_recipes(), e->d_name) >= (int)sizeof rpath)
			continue;
		if (stat(rpath, &st) != 0 || (size_t)st.st_size > RECIPE_FILE_CAP)
			continue;
		f = fopen(rpath, "r");
		if (!f)
			continue;
		text = malloc((size_t)st.st_size + 1);
		if (!text) {
			fclose(f);
			continue;
		}
		got = fread(text, 1, (size_t)st.st_size, f);
		fclose(f);
		text[got] = 0;
		if (nn == cap) {
			size_t nc = cap ? cap * 2 : 64;
			recipe_t *p = realloc(arr, nc * sizeof *arr);
			if (!p) {
				free(text);
				break;
			}
			arr = p;
			cap = nc;
		}
		r = &arr[nn];
		memset(r, 0, sizeof *r);
		recipe_field(text, "pkgname", r->name, sizeof r->name);
		recipe_field(text, "pkgver", r->ver, sizeof r->ver);
		recipe_field(text, "pkgrel", r->rel, sizeof r->rel);
		recipe_field(text, "pkgdesc", r->desc, sizeof r->desc);
		recipe_field(text, "license", r->license, sizeof r->license);
		recipe_field(text, "origin", r->origin, sizeof r->origin);
		recipe_field(text, "subpackages", r->sub, sizeof r->sub);
		recipe_field(text, "disabled", tmp, sizeof tmp);
		r->disabled = recipe_disabled_value(tmp);
		{
			char dep[4096];
			recipe_field(text, "depends", dep, sizeof dep);
			r->depends = dep[0] ? strdup(dep) : NULL;
			if (dep[0] && !r->depends) {
				free(text);
				break;
			}
		}
		free(text);
		if (!r->name[0])
			continue;
		nn++;
	}
	closedir(d);
	*out = arr;
	*n = nn;
	return 0;
}

static void recipe_free_all(recipe_t *arr, size_t n) {
	size_t i;
	if (!arr)
		return;
	for (i = 0; i < n; i++)
		free(arr[i].depends);
	free(arr);
}

/* ------------------------------------------------------------------ */
/* package-name set + NVR label stripping                              */
/* ------------------------------------------------------------------ */
typedef struct {
	char **names;
	size_t n, cap;
} nameset_t;

static void nameset_free(nameset_t *s) {
	size_t i;
	for (i = 0; i < s->n; i++)
		free(s->names[i]);
	free(s->names);
	s->names = NULL;
	s->n = s->cap = 0;
}

static int nameset_add(nameset_t *s, const char *name) {
	size_t i;
	char *copy;
	if (!name || !name[0])
		return 0;
	for (i = 0; i < s->n; i++) {
		if (strcmp(s->names[i], name) == 0)
			return 1;
	}
	if (s->n >= 16384)
		return 0;
	if (s->n == s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 256;
		char **p = realloc(s->names, nc * sizeof *p);
		if (!p)
			return 0;
		s->names = p;
		s->cap = nc;
	}
	copy = strdup(name);
	if (!copy)
		return 0;
	s->names[s->n++] = copy;
	return 1;
}

/* Strip "name-version-revision" labels using the longest known-name
 * prefix. The remainder is kept whole (version strings contain
 * dashes, so splitting version from revision is ambiguous and this
 * code refuses to guess). Returns 1 on a known match, 0 with
 * name=label and empty rest. */
static int strip_nvr(const char *label, nameset_t *ns,
                     char *name, size_t namesz,
                     char *rest, size_t restsz) {
	size_t i, best = 0;
	const char *best_at = NULL;
	for (i = 0; i < ns->n; i++) {
		size_t ln = strlen(ns->names[i]);
		if (ln > best && strncmp(label, ns->names[i], ln) == 0 &&
		    label[ln] == '-') {
			best = ln;
			best_at = ns->names[i];
		}
	}
	if (!best_at) {
		size_t ll = strlen(label);
		if (ll >= namesz)
			ll = namesz - 1;
		memcpy(name, label, ll);
		name[ll] = 0;
		if (restsz)
			rest[0] = 0;
		return 0;
	}
	{
		size_t ll = strlen(best_at);
		size_t rl = strlen(label + ll + 1);
		if (ll >= namesz)
			ll = namesz - 1;
		memcpy(name, best_at, ll);
		name[ll] = 0;
		if (rl >= restsz)
			rl = restsz - 1;
		memcpy(rest, label + ll + 1, rl);
		rest[rl] = 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* dependency graph from `apk dot`                                     */
/* ------------------------------------------------------------------ */
typedef struct {
	int from, to;
} edge_t;

typedef struct {
	char **nodes;
	size_t nn, ncap;
	edge_t *edges;
	size_t ne, ecap;
} graph_t;

#define GRAPH_MAX_NODES 20000u
#define GRAPH_MAX_EDGES 60000u

static void graph_free(graph_t *g) {
	size_t i;
	for (i = 0; i < g->nn; i++)
		free(g->nodes[i]);
	free(g->nodes);
	free(g->edges);
	g->nodes = NULL;
	g->edges = NULL;
	g->nn = g->ncap = g->ne = g->ecap = 0;
}

static int graph_node(graph_t *g, const char *name) {
	size_t i;
	for (i = 0; i < g->nn; i++) {
		if (strcmp(g->nodes[i], name) == 0)
			return (int)i;
	}
	if (g->nn >= GRAPH_MAX_NODES)
		return -1;
	if (g->nn == g->ncap) {
		size_t nc = g->ncap ? g->ncap * 2 : 256;
		char **p = realloc(g->nodes, nc * sizeof *p);
		if (!p)
			return -1;
		g->nodes = p;
		g->ncap = nc;
	}
	g->nodes[g->nn] = strdup(name);
	if (!g->nodes[g->nn])
		return -1;
	return (int)(g->nn++);
}

static int graph_add_edge(graph_t *g, const char *a, const char *b) {
	int ia, ib;
	if (g->ne >= GRAPH_MAX_EDGES)
		return -1;
	ia = graph_node(g, a);
	ib = graph_node(g, b);
	if (ia < 0 || ib < 0)
		return -1;
	if (g->ne == g->ecap) {
		size_t nc = g->ecap ? g->ecap * 2 : 256;
		edge_t *p;
		if (nc > GRAPH_MAX_EDGES)
			nc = GRAPH_MAX_EDGES;
		if (nc <= g->ecap)
			return -1;
		p = realloc(g->edges, nc * sizeof *p);
		if (!p)
			return -1;
		g->edges = p;
		g->ecap = nc;
	}
	g->edges[g->ne].from = ia;
	g->edges[g->ne].to = ib;
	g->ne++;
	return 0;
}

/* Parse `"A" -> "B"` edge lines from apk dot output. Labels over
 * 256 bytes are skipped (bounded, counted). Returns parsed count,
 * or -1 when nothing parsed at all. */
static long graph_parse(const char *dot, nameset_t *ns, graph_t *g,
                        int *skipped) {
	long parsed = 0;
	const char *p = dot;
	*skipped = 0;
	while (*p) {
		const char *nl = strchr(p, '\n');
		const char *eol = nl ? nl : p + strlen(p);
		const char *q1 = memchr(p, '"', (size_t)(eol - p));
		if (q1) {
			const char *q2 = memchr(q1 + 1, '"', (size_t)(eol - (q1 + 1)));
			if (q2) {
				const char *arr = q2 + 1;
				while (arr < eol && (*arr == ' ' || *arr == '\t'))
					arr++;
				if ((size_t)(eol - arr) >= 2 && arr[0] == '-' && arr[1] == '>') {
					const char *q3 = memchr(arr + 2, '"', (size_t)(eol - (arr + 2)));
					if (q3) {
						const char *q4 = memchr(q3 + 1, '"', (size_t)(eol - (q3 + 1)));
						if (q4) {
							char la[257], lb[257];
							char na[129], nb[129], ra[129], rb[129];
							size_t alen = (size_t)(q2 - (q1 + 1));
							size_t blen = (size_t)(q4 - (q3 + 1));
							if (alen >= sizeof la || blen >= sizeof lb) {
								(*skipped)++;
							} else {
								memcpy(la, q1 + 1, alen);
								la[alen] = 0;
								memcpy(lb, q3 + 1, blen);
								lb[blen] = 0;
								strip_nvr(la, ns, na, sizeof na, ra, sizeof ra);
								strip_nvr(lb, ns, nb, sizeof nb, rb, sizeof rb);
								if (graph_add_edge(g, na, nb) == 0)
									parsed++;
								else
									(*skipped)++;
							}
						}
					}
				}
			}
		}
		if (!nl)
			break;
		p = nl + 1;
	}
	if (parsed == 0)
		return -1;
	return parsed;
}

/* BFS reachable set. reverse=0 follows out-edges (dependencies),
 * reverse=1 follows in-edges (reverse dependencies). Prints
 * `tag: <node>` per reachable node (excluding the start), newest
 * first discovered. Caps printed nodes; the cap is reported, never
 * silent. Returns printed count, or -1 when start is unknown. */
static long graph_reach(graph_t *g, int start, int reverse, gbuf *out,
                        const char *tag, long cap, int *truncated) {
	int *queue, *seen;
	size_t qh = 0, qt = 0;
	long printed = 0;
	size_t i;
	*truncated = 0;
	if (start < 0)
		return -1;
	queue = malloc((g->nn + 1) * sizeof *queue);
	seen = calloc(g->nn ? g->nn : 1, sizeof *seen);
	if (!queue || !seen) {
		free(queue);
		free(seen);
		return -1;
	}
	queue[qt++] = start;
	seen[start] = 1;
	while (qh < qt) {
		int cur = queue[qh++];
		for (i = 0; i < g->ne; i++) {
			int nxt = -1;
			if (!reverse && g->edges[i].from == cur)
				nxt = g->edges[i].to;
			else if (reverse && g->edges[i].to == cur)
				nxt = g->edges[i].from;
			if (nxt < 0 || seen[nxt])
				continue;
			seen[nxt] = 1;
			queue[qt++] = nxt;
			if (printed >= cap) {
				*truncated = 1;
				continue;
			}
			gbuf_adds(out, tag);
			gbuf_adds(out, g->nodes[nxt]);
			gbuf_addch(out, '\n');
			printed++;
		}
		if (out->failed)
			break;
	}
	free(queue);
	free(seen);
	return printed;
}

/* DFS enumeration of simple paths from src to dst (node indexes),
 * depth-capped, expansion-capped. Prints up to maxpaths chains.
 * Returns paths printed, with *stopped set when caps cut the search
 * (reported, never silent). */
#define WHY_MAX_DEPTH 15
#define WHY_MAX_PATHS 5
#define WHY_MAX_EXPANSIONS 50000

typedef struct {
	graph_t *g;
	int dst;
	int *onpath;
	int *stack;
	int depth;
	long expansions;
	int maxed;
	gbuf *out;
	long printed;
} dfs_t;

static void dfs_visit(dfs_t *d, int cur) {
	size_t i, k;
	if (d->maxed || d->printed >= WHY_MAX_PATHS)
		return;
	if (cur == d->dst) {
		gbuf_adds(d->out, "chain: ");
		for (k = 0; k < (size_t)d->depth; k++) {
			if (k)
				gbuf_adds(d->out, " -> ");
			gbuf_adds(d->out, d->g->nodes[d->stack[k]]);
		}
		/* dst itself is never pushed (the check precedes the
		 * push), so name the endpoint explicitly: a chain that
		 * stops one hop short answers the wrong question. */
		if (d->depth)
			gbuf_adds(d->out, " -> ");
		gbuf_adds(d->out, d->g->nodes[cur]);
		gbuf_adds(d->out, "\n");
		d->printed++;
		return;
	}
	if (d->depth >= WHY_MAX_DEPTH)
		return;
	d->onpath[cur] = 1;
	d->stack[d->depth++] = cur;
	for (i = 0; i < d->g->ne; i++) {
		if (d->g->edges[i].from != cur)
			continue;
		if (d->expansions++ >= WHY_MAX_EXPANSIONS) {
			d->maxed = 1;
			break;
		}
		if (!d->onpath[d->g->edges[i].to])
			dfs_visit(d, d->g->edges[i].to);
		if (d->maxed || d->printed >= WHY_MAX_PATHS)
			break;
	}
	d->depth--;
	d->onpath[cur] = 0;
}

/* ------------------------------------------------------------------ */
/* build verdicts: dashboard classify semantics, ported verbatim       */
/* ------------------------------------------------------------------ */
#define TRUNC_DETAIL 140

static void shorten_to(gbuf *b, const char *s) {
	size_t n = strlen(s);
	if (n <= TRUNC_DETAIL) {
		gbuf_adds(b, s);
		return;
	}
	gbuf_addn(b, s, TRUNC_DETAIL);
	gbuf_adds(b, "…");
}

static int status_is_fail(const char *s) {
	char tmp[16];
	size_t i;
	for (i = 0; i + 1 < sizeof tmp && s[i]; i++)
		tmp[i] = (char)tolower((unsigned char)s[i]);
	tmp[i] = 0;
	return strcmp(tmp, "fail") == 0 || strcmp(tmp, "failed") == 0 ||
	       strcmp(tmp, "failure") == 0 || strcmp(tmp, "error") == 0;
}

/* First failed stage in dict order ("STAGE: detail"), or "".
 * Dict order is pipeline order, so this names the earliest failed
 * stage - never the build step when a later stage is what failed. */
static void failed_stage_detail(jv *obj, gbuf *detail) {
	jv *stages = jv_get(obj, "stages");
	int i;
	if (!stages || stages->t != 5)
		return;
	for (i = 0; i < stages->nk; i++) {
		jv *entry = stages->kv[i].v;
		jv *st, *dl;
		if (!entry || entry->t != 5)
			continue;
		st = jv_get(entry, "status");
		if (!st || st->t != 3 || !st->str || !status_is_fail(st->str))
			continue;
		gbuf_adds(detail, stages->kv[i].k);
		gbuf_adds(detail, ": ");
		dl = jv_get(entry, "detail");
		if (dl && dl->t == 3 && dl->str)
			shorten_to(detail, dl->str);
		else
			gbuf_adds(detail, "(no detail)");
		return;
	}
}

/* Classify one build row. state/result/finished/started/published are
 * raw column values (NULL allowed). Returns the css-class equivalent:
 * "failed", "refused", "amber" or "ok"; appends the state text and
 * detail. Unknown terminal states read FAILED with whatever detail
 * the result carries; unknown input shapes never break the caller. */
static const char *classify_build(const char *state, const char *result_json,
                                  int finished_null, int started_null,
                                  int published_null,
                                  gbuf *state_text, gbuf *detail) {
	jv *obj;
	if (finished_null) {
		gbuf_adds(state_text, started_null ? "QUEUED" : "RUNNING");
		return "amber";
	}
	obj = (result_json && result_json[0]) ? json_parse(result_json, strlen(result_json)) : NULL;
	if (obj) {
		jv *att = jv_get(obj, "attempt");
		if (att && att->t == 3 && att->str && strcmp(att->str, "refusal") == 0) {
			jv *stages, *build, *dl;
			gbuf_adds(state_text, "REFUSED");
			gbuf_adds(detail, "never built");
			stages = jv_get(obj, "stages");
			build = (stages && stages->t == 5) ? jv_get(stages, "BUILD") : NULL;
			dl = (build && build->t == 5) ? jv_get(build, "detail") : NULL;
			if (dl && dl->t == 3 && dl->str) {
				gbuf_adds(detail, ": ");
				shorten_to(detail, dl->str);
			}
			jv_free(obj);
			return "refused";
		}
	}
	if (state && strcmp(state, "PASS") == 0) {
		/* published_at is written when the package publishes. A PASS
		 * without one is built but awaiting signing. If the column
		 * were ever left unpopulated, every PASS would read amber:
		 * visibly wrong, reported, one-line fix - never silent. */
		gbuf_adds(state_text, published_null ? "AWAITING SIGNING" : "PUBLISHED");
		jv_free(obj);
		return published_null ? "amber" : "ok";
	}
	{
		gbuf_adds(state_text, "FAILED");
		if (obj) {
			failed_stage_detail(obj, detail);
			if (!detail->len) {
				jv *bl = jv_get(obj, "blockers");
				if (bl && bl->t == 4 && bl->ni > 0) {
					int i;
					gbuf_adds(detail, "blockers: ");
					for (i = 0; i < bl->ni; i++) {
						jv *e = bl->items[i];
						if (i)
							gbuf_adds(detail, ", ");
						if (e && e->t == 3 && e->str)
							shorten_to(detail, e->str);
						else
							gbuf_adds(detail, "?");
					}
				} else {
					jv *att = jv_get(obj, "attempt");
					if (att && att->t == 3 && att->str)
						shorten_to(detail, att->str);
					else
						gbuf_adds(detail, "failed (no detail recorded)");
				}
			}
		} else if (!detail->len) {
			gbuf_adds(detail, "failed (no detail recorded)");
		}
		jv_free(obj);
		return "failed";
	}
}

/* SBU suffix for one build row: " (2.1 SBU)" when an active baseline
 * matches, else nothing. Never fails the row. */
static void sbu_suffix(sqlite3 *db, const char *package, const char *arch,
                       const char *host, double wall, gbuf *b) {
	sqlite3_stmt *st = NULL;
	if (!package || !arch || !host || wall <= 0)
		return;
	if (sqlite3_prepare_v2(db,
	    "SELECT wall_s FROM sbu_baseline WHERE package=?1 AND arch=?2"
	    " AND builder_host=?3 AND active=1", -1, &st, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(st, 1, package, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, arch, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, host, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW &&
	    sqlite3_column_type(st, 0) != SQLITE_NULL) {
		double base = sqlite3_column_double(st, 0);
		if (base > 0) {
			char tmp[32];
			snprintf(tmp, sizeof tmp, " (%.1f SBU)", wall / base);
			gbuf_adds(b, tmp);
		}
	}
	sqlite3_finalize(st);
}

/* ------------------------------------------------------------------ */
/* shared lookups                                                      */
/* ------------------------------------------------------------------ */
static recipe_t *find_recipe(recipe_t *pkgs, size_t n, const char *name) {
	size_t i;
	for (i = 0; i < n; i++) {
		if (strcmp(pkgs[i].name, name) == 0)
			return &pkgs[i];
	}
	return NULL;
}

/* Split "name-version-revision" remainder display: "2.14-r3". */
static void nvr_rest(const char *ver, const char *rel, char *out, size_t outsz) {
	snprintf(out, outsz, "%s-r%s", ver, rel);
}

static void prov(gbuf *b, const char *src) {
	gbuf_adds(b, "\n[src: ");
	gbuf_adds(b, src);
	gbuf_adds(b, "]");
}

/* ------------------------------------------------------------------ */
/* package_search                                                      */
/* ------------------------------------------------------------------ */
static void like_escape(gbuf *b, const char *s) {
	for (; *s; s++) {
		if (*s == '%' || *s == '_' || *s == '\\')
			gbuf_addch(b, '\\');
		gbuf_addch(b, *s);
	}
}

static int t_search(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *q;
	long limit;
	int bad = 0, incdis;
	recipe_t *pkgs = NULL;
	size_t n = 0, i, shown = 0;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char dberr[256];
	char dbpath[4096];
	(void)err;
	(void)errsz;
	q = arg_str(args, "query");
	if (!q || !q[0])
		return -1;
	limit = clamp_long(arg_long(args, "limit", 20, &bad), 1, 200);
	if (bad)
		return -1;
	incdis = arg_bool(args, "include_disabled", 0);
	if (incdis < 0)
		return -1;
	if (recipe_collect(&pkgs, &n) != 0) {
		*is_error = 1;
		gbuf_adds(text, "cannot list recipes");
		prov(text, "recipes");
		return 0;
	}
	gbuf_adds(text, "recipes:\n");
	for (i = 0; i < n && shown < (size_t)limit; i++) {
		if (pkgs[i].disabled && !incdis)
			continue;
		if (!cistrstr(pkgs[i].name, q) && !cistrstr(pkgs[i].desc, q))
			continue;
		gbuf_adds(text, pkgs[i].name);
		gbuf_addch(text, ' ');
		gbuf_adds(text, pkgs[i].ver);
		gbuf_adds(text, "-r");
		gbuf_adds(text, pkgs[i].rel);
		if (pkgs[i].disabled)
			gbuf_adds(text, " [disabled]");
		gbuf_adds(text, " - ");
		gbuf_adds(text, pkgs[i].desc[0] ? pkgs[i].desc : "no description");
		gbuf_addch(text, '\n');
		shown++;
	}
	if (shown == (size_t)limit)
		gbuf_adds(text, "(capped: refine the query)\n");
	recipe_free_all(pkgs, n);
	gbuf_adds(text, "repository:\n");
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db", path_repo_dir()) >= (int)sizeof dbpath) {
		gbuf_adds(text, "(repository.db path too long)\n");
	} else if ((db = db_open_ro(dbpath, dberr, sizeof dberr)) == NULL) {
		gbuf_adds(text, "(repository unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_adds(text, ")\n");
	} else {
		gbuf pat;
		int rc;
		gbuf_init(&pat);
		gbuf_addch(&pat, '%');
		like_escape(&pat, q);
		gbuf_addch(&pat, '%');
		rc = sqlite3_prepare_v2(db,
		    "SELECT name, nvr FROM packages WHERE name LIKE ?1 ESCAPE '\\' AND is_newest=1"
		    " ORDER BY name LIMIT ?2", -1, &st, NULL);
		if (rc != SQLITE_OK || pat.failed) {
			gbuf_adds(text, "(repository query failed)\n");
		} else {
			size_t rshown = 0;
			sqlite3_bind_text(st, 1, pat.p ? pat.p : "%", -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(st, 2, (int)limit);
			while ((rc = sqlite3_step(st)) == SQLITE_ROW && rshown < (size_t)limit) {
				sb_cell(text, st, 0);
				gbuf_addch(text, ' ');
				sb_cell(text, st, 1);
				gbuf_addch(text, '\n');
				rshown++;
			}
			if (rshown == 0)
				gbuf_adds(text, "(no repository match)\n");
			else if (rshown == (size_t)limit)
				gbuf_adds(text, "(capped: refine the query)\n");
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
		gbuf_free(&pat);
	}
	prov(text, "recipes, repository.db");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_info                                                        */
/* ------------------------------------------------------------------ */
static int t_info(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name;
	recipe_t *pkgs = NULL;
	size_t n = 0;
	recipe_t *r;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char dberr[256];
	char dbpath[4096];
	int have_recipe = 0, have_repo = 0;
	(void)err;
	(void)errsz;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	if (recipe_collect(&pkgs, &n) != 0) {
		*is_error = 1;
		gbuf_adds(text, "cannot list recipes");
		prov(text, "recipes");
		return 0;
	}
	r = find_recipe(pkgs, n, name);
	if (r) {
		char nvr[280];
		have_recipe = 1;
		gbuf_adds(text, "name: ");
		gbuf_adds(text, r->name);
		gbuf_adds(text, "\nversion: ");
		gbuf_adds(text, r->ver[0] ? r->ver : "-");
		gbuf_adds(text, "\nrevision: ");
		gbuf_adds(text, r->rel[0] ? r->rel : "-");
		nvr_rest(r->ver[0] ? r->ver : "?", r->rel[0] ? r->rel : "?", nvr, sizeof nvr);
		gbuf_adds(text, "\ndescription: ");
		gbuf_adds(text, r->desc[0] ? r->desc : "-");
		gbuf_adds(text, "\nlicense: ");
		gbuf_adds(text, r->license[0] ? r->license : "-");
		gbuf_adds(text, "\norigin: ");
		gbuf_adds(text, r->origin[0] ? r->origin : "-");
		gbuf_adds(text, "\ndisabled: ");
		gbuf_adds(text, r->disabled ? "yes" : "no");
		gbuf_adds(text, "\nsubpackages: ");
		gbuf_adds(text, r->sub[0] ? r->sub : "-");
		gbuf_adds(text, "\n");
	}
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db", path_repo_dir()) < (int)sizeof dbpath &&
	    (db = db_open_ro(dbpath, dberr, sizeof dberr)) != NULL) {
		if (sqlite3_prepare_v2(db,
		    "SELECT nvr, published_at FROM packages WHERE name=?1 AND is_newest=1",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW) {
				have_repo = 1;
				gbuf_adds(text, "repo newest: ");
				sb_cell(text, st, 0);
				gbuf_adds(text, " (published ");
				sb_cell(text, st, 1);
				gbuf_adds(text, ")\n");
			}
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	}
	if (!have_recipe && !have_repo) {
		*is_error = 1;
		gbuf_adds(text, "unknown package: ");
		gbuf_adds(text, name);
		gbuf_addch(text, '\n');
	} else if (!have_recipe) {
		gbuf_adds(text, "(no recipe in tree)\n");
	} else if (!have_repo) {
		gbuf_adds(text, "(not in repository)\n");
	}
	recipe_free_all(pkgs, n);
	prov(text, "recipes, repository.db");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* dependency graph via `apk dot`                                      */
/* ------------------------------------------------------------------ */
static void nameset_load_all(nameset_t *ns) {
	recipe_t *pkgs = NULL;
	size_t n = 0, i;
	char dbpath[4096];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	if (recipe_collect(&pkgs, &n) == 0) {
		for (i = 0; i < n; i++)
			nameset_add(ns, pkgs[i].name);
		recipe_free_all(pkgs, n);
	}
	/* Best-effort enrichment: failures degrade to unparsed labels,
	 * never to a failed tool (labels still display whole). */
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db", path_repo_dir()) >= (int)sizeof dbpath)
		return;
	{
		char dberr[64];
		db = db_open_ro(dbpath, dberr, sizeof dberr);
	}
	if (!db)
		return;
	if (sqlite3_prepare_v2(db, "SELECT DISTINCT name FROM packages",
	    -1, &st, NULL) == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW) {
			const char *nm = (const char *)sqlite3_column_text(st, 0);
			if (nm)
				nameset_add(ns, nm);
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
}

/* First line of stderr (bounded), for "live graph unavailable" notes. */
static void stderr_first(gbuf *errput, char *out, size_t outsz) {
	size_t i = 0, j = 0;
	while (i < errput->len && j + 1 < outsz && i < 200) {
		if (errput->p[i] == '\n')
			break;
		out[j++] = errput->p[i++];
	}
	out[j] = 0;
	if (!j)
		snprintf(out, outsz, "no detail");
}

static int t_deps(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name, *dir;
	int rev = 0, both = 0;
	nameset_t ns;
	graph_t g;
	gbuf dot, dberr;
	char ebuf[256];
	char *av[4];
	int rc, skipped = 0;
	memset(&ns, 0, sizeof ns);
	memset(&g, 0, sizeof g);
	gbuf_init(&dot);
	gbuf_init(&dberr);
	name = arg_str(args, "name");
	dir = arg_str(args, "direction");
	if (!name || !name[0])
		return -1;
	if (!dir || !dir[0])
		dir = "deps";
	if (strcmp(dir, "reverse") == 0)
		rev = 1;
	else if (strcmp(dir, "both") == 0)
		both = 1;
	else if (strcmp(dir, "deps") != 0)
		return -1;
	nameset_load_all(&ns);
	/* Reverse-only skips the deps section and uses the else branch
	 * below; both runs deps here plus reverse in the nested block. */
	if (!rev) {
		av[0] = "apk";
		av[1] = "dot";
		av[2] = (char *)name;
		av[3] = NULL;
		rc = run_capture(av, &dot, &dberr, &skipped, ebuf, sizeof ebuf);
		if (rc != 0 || dot.failed) {
			/* Fallback: recipe-declared deps, explicitly labeled
			 * unresolved (covers unpublished recipes the live
			 * graph cannot know). */
			recipe_t *pkgs = NULL;
			size_t nn = 0;
			recipe_t *r;
			char why[220];
			if (rc != 0)
				stderr_first(&dberr, why, sizeof why);
			else
				snprintf(why, sizeof why, "output error");
			gbuf_adds(text, "live graph unavailable (apk dot: ");
			gbuf_adds(text, why);
			gbuf_adds(text, ")\ndeclared (unresolved):\n");
			if (recipe_collect(&pkgs, &nn) == 0 && (r = find_recipe(pkgs, nn, name)) != NULL && r->depends) {
				char *tok, *dup = strdup(r->depends);
				if (dup) {
					for (tok = strtok(dup, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
						gbuf_adds(text, "dep: ");
						gbuf_adds(text, tok);
						gbuf_addch(text, '\n');
					}
					free(dup);
				}
			} else {
				gbuf_adds(text, "(no recipe declaration either)\n");
			}
			recipe_free_all(pkgs, nn);
			nameset_free(&ns);
			graph_free(&g);
			gbuf_free(&dot);
			gbuf_free(&dberr);
			prov(text, "recipes");
			(void)err;
			(void)errsz;
			return text->failed ? -1 : 0;
		}
		if (graph_parse(dot.p ? dot.p : "", &ns, &g, &skipped) < 0) {
			*is_error = 1;
			gbuf_adds(text, "no edges parsed from apk output");
			goto done_deps;
		}
		{
			int start = -1;
			size_t ii;
			for (ii = 0; ii < g.nn; ii++) {
				if (strcmp(g.nodes[ii], name) == 0) {
					start = (int)ii;
					break;
				}
			}
			if (both || !rev) {
				long printed; int trunc = 0;
				gbuf_adds(text, "dependencies:\n");
				if (start < 0) {
					gbuf_adds(text, "(package not in live graph)\n");
				} else {
					printed = graph_reach(&g, start, 0, text, "dep: ", 500, &trunc);
					if (printed == 0)
						gbuf_adds(text, "(no dependencies)\n");
					else if (trunc)
						gbuf_adds(text, "(truncated at 500)\n");
				}
			}
			if (both || rev) {
				/* Reverse needs the full graph, not one closure. */
				gbuf full, ferr;
				char *av2[3];
				int rc2, sk2 = 0;
				gbuf_init(&full);
				gbuf_init(&ferr);
				av2[0] = "apk";
				av2[1] = "dot";
				av2[2] = NULL;
				graph_free(&g);
				memset(&g, 0, sizeof g);
				rc2 = run_capture(av2, &full, &ferr, &sk2, ebuf, sizeof ebuf);
				if (rc2 != 0 || full.failed ||
				    graph_parse(full.p ? full.p : "", &ns, &g, &sk2) < 0) {
					gbuf_adds(text, "reverse dependencies unavailable\n");
				} else {
					int rs = -1;
					size_t jj;
					for (jj = 0; jj < g.nn; jj++) {
						if (strcmp(g.nodes[jj], name) == 0) {
							rs = (int)jj;
							break;
						}
					}
					gbuf_adds(text, "reverse dependencies:\n");
					if (rs < 0) {
						gbuf_adds(text, "(package not in live graph)\n");
					} else {
						long printed; int trunc = 0;
						printed = graph_reach(&g, rs, 1, text, "rdep: ", 500, &trunc);
						if (printed == 0)
							gbuf_adds(text, "(no reverse dependencies)\n");
						else if (trunc)
							gbuf_adds(text, "(truncated at 500)\n");
					}
				}
				gbuf_free(&full);
				gbuf_free(&ferr);
			}
		}
	} else {
		/* reverse-only: same full-graph path without the deps section. */
		gbuf full, ferr;
		char *av2[3];
		int rc2, sk2 = 0;
		gbuf_init(&full);
		gbuf_init(&ferr);
		av2[0] = "apk";
		av2[1] = "dot";
		av2[2] = NULL;
		rc2 = run_capture(av2, &full, &ferr, &sk2, ebuf, sizeof ebuf);
		if (rc2 != 0 || full.failed ||
		    graph_parse(full.p ? full.p : "", &ns, &g, &sk2) < 0) {
			gbuf_adds(text, "reverse dependencies unavailable\n");
		} else {
			int rs = -1;
			size_t jj;
			for (jj = 0; jj < g.nn; jj++) {
				if (strcmp(g.nodes[jj], name) == 0) {
					rs = (int)jj;
					break;
				}
			}
			gbuf_adds(text, "reverse dependencies:\n");
			if (rs < 0) {
				gbuf_adds(text, "(package not in live graph)\n");
			} else {
				long printed; int trunc = 0;
				printed = graph_reach(&g, rs, 1, text, "rdep: ", 500, &trunc);
				if (printed == 0)
					gbuf_adds(text, "(no reverse dependencies)\n");
				else if (trunc)
					gbuf_adds(text, "(truncated at 500)\n");
			}
		}
		gbuf_free(&full);
		gbuf_free(&ferr);
	}
done_deps:
	nameset_free(&ns);
	graph_free(&g);
	gbuf_free(&dot);
	gbuf_free(&dberr);
	prov(text, "apk dot");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_why_installed                                               */
/* ------------------------------------------------------------------ */
static int dep_in_declared(const char *pkg, const char *dep) {
	recipe_t *pkgs = NULL;
	size_t n = 0;
	recipe_t *r;
	int found = 0;
	if (recipe_collect(&pkgs, &n) != 0)
		return 0;
	r = find_recipe(pkgs, n, pkg);
	if (r && r->depends) {
		char *dup = strdup(r->depends), *tok;
		if (dup) {
			for (tok = strtok(dup, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
				size_t tl = strcspn(tok, "<>=");
				char tmp[128];
				if (tl >= sizeof tmp)
					continue;
				memcpy(tmp, tok, tl);
				tmp[tl] = 0;
				if (strcmp(tmp, dep) == 0) {
					found = 1;
					break;
				}
			}
			free(dup);
		}
	}
	recipe_free_all(pkgs, n);
	return found;
}

static int t_why(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg, *dep;
	nameset_t ns;
	graph_t g;
	gbuf dot, dberr;
	char ebuf[256];
	char *av[3];
	int rc, sk = 0, src = -1, dst = -1;
	int skipped = 0;
	size_t i;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	dep = arg_str(args, "dep");
	if (!pkg || !pkg[0] || !dep || !dep[0])
		return -1;
	memset(&ns, 0, sizeof ns);
	memset(&g, 0, sizeof g);
	gbuf_init(&dot);
	gbuf_init(&dberr);
	nameset_load_all(&ns);
	av[0] = "apk";
	av[1] = "dot";
	av[2] = NULL;
	rc = run_capture(av, &dot, &dberr, &sk, ebuf, sizeof ebuf);
	if (rc != 0 || dot.failed || graph_parse(dot.p ? dot.p : "", &ns, &g, &skipped) < 0) {
		if (dep_in_declared(pkg, dep)) {
			gbuf_adds(text, "declared directly (unresolved graph): ");
			gbuf_adds(text, pkg);
			gbuf_adds(text, " -> ");
			gbuf_adds(text, dep);
			gbuf_addch(text, '\n');
		} else {
			*is_error = 1;
			gbuf_adds(text, "dependency graph unavailable and no direct declaration found");
		}
		goto done_why;
	}
	for (i = 0; i < g.nn; i++) {
		if (strcmp(g.nodes[i], pkg) == 0)
			src = (int)i;
		if (strcmp(g.nodes[i], dep) == 0)
			dst = (int)i;
	}
	/* Short dependency names (libbar) address NVR-labelled graph
	 * nodes (libbar-0.1-r1) that no registry knows: strip_nvr
	 * deliberately refuses to guess those, so the lookup itself
	 * accepts an exact match or a `dep-` prefix. Exact wins; the
	 * first prefix hit in graph order wins otherwise. */
	if (dst < 0) {
		size_t dl = strlen(dep);
		for (i = 0; i < g.nn; i++) {
			if (strncmp(g.nodes[i], dep, dl) == 0 &&
			    g.nodes[i][dl] == '-') {
				dst = (int)i;
				break;
			}
		}
	}
	if (src < 0) {
		if (dep_in_declared(pkg, dep)) {
			gbuf_adds(text, "declared directly (unresolved graph): ");
			gbuf_adds(text, pkg);
			gbuf_adds(text, " -> ");
			gbuf_adds(text, dep);
			gbuf_addch(text, '\n');
		} else {
			*is_error = 1;
			gbuf_adds(text, "unknown package in dependency graph: ");
			gbuf_adds(text, pkg);
			gbuf_addch(text, '\n');
		}
		goto done_why;
	}
	if (dst < 0 || src == dst) {
		gbuf_adds(text, dep);
		if (dst < 0) {
			gbuf_adds(text, " is not in ");
			gbuf_adds(text, pkg);
			gbuf_adds(text, "'s closure\n");
		} else {
			gbuf_adds(text, " is the package itself\n");
		}
		goto done_why;
	}
	{
		dfs_t d;
		int *onpath, *stack;
		onpath = calloc(g.nn ? g.nn : 1, sizeof *onpath);
		stack = malloc(WHY_MAX_DEPTH * sizeof *stack);
		if (!onpath || !stack) {
			free(onpath);
			free(stack);
			*is_error = 1;
			gbuf_adds(text, "out of memory");
			goto done_why;
		}
		memset(&d, 0, sizeof d);
		d.g = &g;
		d.dst = dst;
		d.onpath = onpath;
		d.stack = stack;
		d.out = text;
		dfs_visit(&d, src);
		if (d.printed == 0)
			gbuf_adds(text, dep),
			gbuf_adds(text, " is not in "),
			gbuf_adds(text, pkg),
			gbuf_adds(text, "'s closure\n");
		else if (d.maxed || d.printed >= WHY_MAX_PATHS)
			gbuf_adds(text, "(up to 5 chains shown)\n");
		free(onpath);
		free(stack);
	}
done_why:
	nameset_free(&ns);
	graph_free(&g);
	gbuf_free(&dot);
	gbuf_free(&dberr);
	prov(text, "apk dot");
	if (text->failed)
		return -1;
	return 0;
}

/* Baseline coverage line: which SBU baselines are active for a
 * package, or an explicit statement that none is. Silence here once
 * hid a completely empty baseline table: every history rendered
 * without SBU ratios and nothing said why. */
static void sbu_baseline_line(sqlite3 *db, const char *pkg, gbuf *text) {
	sqlite3_stmt *st = NULL;
	int rows = 0;
	if (sqlite3_prepare_v2(db,
	    "SELECT cache_policy,wall_s,arch,builder_host,calibrated_at"
	    " FROM sbu_baseline WHERE package=?1 AND active=1"
	    " ORDER BY arch,builder_host", -1, &st, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
	while (sqlite3_step(st) == SQLITE_ROW) {
		char tmp[128];
		if (!rows)
			gbuf_adds(text, "SBU baseline:");
		gbuf_adds(text, " ");
		sb_cell(text, st, 0);
		snprintf(tmp, sizeof tmp, " %.1fs wall (",
		         sqlite3_column_double(st, 1));
		gbuf_adds(text, tmp);
		sb_cell(text, st, 2);
		gbuf_addch(text, '/');
		sb_cell(text, st, 3);
		gbuf_adds(text, ", calibrated ");
		sb_cell(text, st, 4);
		gbuf_addch(text, ')');
		gbuf_addch(text, '\n');
		rows++;
	}
	sqlite3_finalize(st);
	if (!rows) {
		gbuf_adds(text, "SBU baseline: none active for ");
		gbuf_adds(text, pkg);
		gbuf_adds(text, " (SBU omitted; calibrate to establish)\n");
	}
}

/* ------------------------------------------------------------------ */
/* shared build-row rendering                                          */
/* ------------------------------------------------------------------ */
#define BUILD_COLS \
	"package,version,revision,state,result_json,duration_s,cpu_s," \
	"peak_rss_mb,arch,builder_host,finished_at,started_at,published_at"
/* Column indexes in BUILD_COLS order: 0 package, 1 version,
 * 2 revision, 3 state, 4 result_json, 5 duration_s, 6 cpu_s,
 * 7 peak_rss_mb, 8 arch, 9 builder_host, 10 finished_at,
 * 11 started_at, 12 published_at. */

static const char *col_text(sqlite3_stmt *st, int col, int *is_null) {
	if (sqlite3_column_type(st, col) == SQLITE_NULL) {
		*is_null = 1;
		return NULL;
	}
	*is_null = 0;
	return (const char *)sqlite3_column_text(st, col);
}

/* One build row as `nvr: STATE — detail` plus a timings line. */
static void format_build(sqlite3 *db, sqlite3_stmt *st, gbuf *text, int hint_sign) {
	int n10, n11, n12, n8, n9, unused;
	const char *package, *version, *revision, *state, *result;
	const char *arch, *host;
	gbuf sbuf, dbuf;
	package = col_text(st, 0, &unused);
	version = col_text(st, 1, &unused);
	revision = col_text(st, 2, &unused);
	state = col_text(st, 3, &unused);
	result = col_text(st, 4, &unused);
	arch = col_text(st, 8, &n8);
	host = col_text(st, 9, &n9);
	col_text(st, 10, &unused);
	col_text(st, 11, &unused);
	col_text(st, 12, &unused);
	(void)unused;
	n10 = (sqlite3_column_type(st, 10) == SQLITE_NULL);
	n11 = (sqlite3_column_type(st, 11) == SQLITE_NULL);
	n12 = (sqlite3_column_type(st, 12) == SQLITE_NULL);
	gbuf_init(&sbuf);
	gbuf_init(&dbuf);
	{
		const char *cls = classify_build(state, result,
		    n10, n11, n12, &sbuf, &dbuf);
		(void)cls;
	}
	gbuf_adds(text, package ? package : "?");
	gbuf_addch(text, ' ');
	gbuf_adds(text, version ? version : "?");
	gbuf_adds(text, "-r");
	gbuf_adds(text, revision ? revision : "?");
	gbuf_adds(text, ": ");
	gbuf_adds(text, sbuf.p ? sbuf.p : "?");
	if (dbuf.len) {
		gbuf_adds(text, " — ");
		gbuf_adds(text, dbuf.p);
	}
	gbuf_addch(text, '\n');
	gbuf_adds(text, "  wall ");
	sb_cell(text, st, 5);
	gbuf_adds(text, " cpu ");
	sb_cell(text, st, 6);
	gbuf_adds(text, " rss ");
	sb_cell(text, st, 7);
	gbuf_adds(text, " arch ");
	sb_cell(text, st, 8);
	gbuf_adds(text, " builder ");
	sb_cell(text, st, 9);
	gbuf_adds(text, " finished ");
	sb_cell(text, st, 10);
	gbuf_addch(text, '\n');
	/* The signer is a human-gated boundary: the MCP never runs it,
	 * so a staged row names the exact ask instead of going quiet. */
	if (hint_sign && sbuf.p && strstr(sbuf.p, "AWAITING SIGNING")) {
		gbuf_adds(text, "  next: built and unsigned - ask the operator to run `sudo sign-apk-repo ");
		gbuf_adds(text, package ? package : "?");
		gbuf_adds(text, "`\n");
	}
	if (db && !n10 && sqlite3_column_type(st, 5) != SQLITE_NULL &&
	    !n8 && !n9) {
		/* Copy first: the sbu query steps another statement on the
		 * same handle, so column pointers must not cross the call. */
		char pkg[128], ah[64], bh[128];
		snprintf(pkg, sizeof pkg, "%s", package ? package : "");
		snprintf(ah, sizeof ah, "%s", arch ? arch : "");
		snprintf(bh, sizeof bh, "%s", host ? host : "");
		sbu_suffix(db, pkg, ah, bh, sqlite3_column_double(st, 5), text);
		if (text->len && text->p[text->len - 1] != '\n')
			gbuf_addch(text, '\n');
	}
	gbuf_free(&sbuf);
	gbuf_free(&dbuf);
}

/* ------------------------------------------------------------------ */
/* package_installed                                                   */
/* ------------------------------------------------------------------ */
static int t_installed(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name;
	char dbpath[4096];
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	FILE *f;
	char line[4096];
	char iver[256], idesc[256];
	int have_inst = 0, have_repo = 0;
	char nvr[280];
	(void)err;
	(void)errsz;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	iver[0] = 0;
	idesc[0] = 0;
	f = fopen(path_apkdb(), "r");
	if (f) {
		char cn[128] = "", cv[128] = "", cd[256] = "";
		int in_match = 0, matched = 0;
		while (fgets(line, sizeof line, f)) {
			size_t L;
			if (line[0] == '\n') {
				in_match = 0;
				if (matched)
					break;
				cn[0] = cv[0] = cd[0] = 0;
				continue;
			}
			L = strlen(line);
			while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
				line[--L] = 0;
			if (L < 2 || line[1] != ':')
				continue;
			if (line[0] == 'P') {
				in_match = strcmp(line + 2, name) == 0;
				if (in_match)
					matched = 1;
			} else if (in_match && line[0] == 'V') {
				snprintf(cv, sizeof cv, "%s", line + 2);
			} else if (in_match && line[0] == 'T') {
				snprintf(cd, sizeof cd, "%s", line + 2);
			}
		}
		fclose(f);
		if (matched && cv[0]) {
			have_inst = 1;
			snprintf(iver, sizeof iver, "%s", cv);
			snprintf(idesc, sizeof idesc, "%s", cd);
		}
	}
	if (have_inst) {
		gbuf_adds(text, "installed: ");
		gbuf_adds(text, iver);
		if (idesc[0]) {
			gbuf_adds(text, " (");
			gbuf_adds(text, idesc);
			gbuf_adds(text, ")");
		}
		gbuf_addch(text, '\n');
	} else {
		gbuf_adds(text, "installed: none\n");
	}
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db", path_repo_dir()) < (int)sizeof dbpath &&
	    (db = db_open_ro(dbpath, dberr, sizeof dberr)) != NULL) {
		if (sqlite3_prepare_v2(db,
		    "SELECT nvr FROM packages WHERE name=?1 AND is_newest=1",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW &&
			    sqlite3_column_type(st, 0) != SQLITE_NULL) {
				have_repo = 1;
				snprintf(nvr, sizeof nvr, "%s",
				         (const char *)sqlite3_column_text(st, 0));
			}
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	}
	if (have_repo) {
		gbuf_adds(text, "repository: ");
		gbuf_adds(text, nvr);
		gbuf_addch(text, '\n');
	} else {
		gbuf_adds(text, "repository: absent\n");
	}
	if (have_inst && have_repo) {
		gbuf_adds(text, strcmp(iver, nvr) == 0 ? "verdict: up to date\n" : "verdict: differ\n");
	} else if (!have_inst && !have_repo) {
		*is_error = 1;
		gbuf_adds(text, "unknown package: ");
		gbuf_adds(text, name);
		gbuf_addch(text, '\n');
		return 0;
	}
	prov(text, "installed DB, repository.db");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_build_status                                                */
/* ------------------------------------------------------------------ */
static int recipe_is_disabled(const char *name, int *known) {
	recipe_t *pkgs = NULL;
	size_t n = 0, i;
	int dis = 0;
	*known = 0;
	if (recipe_collect(&pkgs, &n) != 0)
		return 0;
	for (i = 0; i < n; i++) {
		if (strcmp(pkgs[i].name, name) == 0) {
			*known = 1;
			dis = pkgs[i].disabled;
			break;
		}
	}
	recipe_free_all(pkgs, n);
	return dis;
}

static int t_status(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name;
	int incdis, known = 0;
	char dbpath[4096];
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	(void)err;
	(void)errsz;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	incdis = arg_bool(args, "include_disabled", 0);
	if (incdis < 0)
		return -1;
	if (recipe_is_disabled(name, &known) && !incdis) {
		gbuf_adds(text, "package ");
		gbuf_adds(text, name);
		gbuf_adds(text, " is disabled (build queries exclude it;"
		                 " rerun with include_disabled=true)\n");
		prov(text, "recipes");
		return text->failed ? -1 : 0;
	}
	if (snprintf(dbpath, sizeof dbpath, "%s", path_queue_db()) >= (int)sizeof dbpath) {
		*is_error = 1;
		gbuf_adds(text, "queue database path too long");
		return 0;
	}
	db = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		return 0;
	}
	if (sqlite3_prepare_v2(db,
	    "SELECT package,version,revision,state,result_json,duration_s,cpu_s,"
	    "peak_rss_mb,arch,builder_host,finished_at,started_at,published_at"
	    " FROM builds WHERE package=?1"
	    " ORDER BY finished_at IS NULL DESC, finished_at DESC LIMIT 1",
	    -1, &st, NULL) != SQLITE_OK) {
		*is_error = 1;
		gbuf_adds(text, "queue query failed");
		sqlite3_close(db);
		return 0;
	}
	(void)dbpath;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		format_build(db, st, text, 1);
	} else {
		gbuf_adds(text, "never built: ");
		gbuf_adds(text, name);
		gbuf_addch(text, '\n');
	}
	sbu_baseline_line(db, name, text);
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "build queue");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_build_history                                               */
/* ------------------------------------------------------------------ */
static int t_history(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name;
	int incdis, known = 0, bad = 0;
	long limit;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	long shown = 0;
	int rc;
	(void)err;
	(void)errsz;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	limit = clamp_long(arg_long(args, "limit", 10, &bad), 1, 100);
	if (bad)
		return -1;
	incdis = arg_bool(args, "include_disabled", 0);
	if (incdis < 0)
		return -1;
	if (recipe_is_disabled(name, &known) && !incdis) {
		gbuf_adds(text, "package ");
		gbuf_adds(text, name);
		gbuf_adds(text, " is disabled (build queries exclude it;"
		                 " rerun with include_disabled=true)\n");
		prov(text, "recipes");
		return text->failed ? -1 : 0;
	}
	db = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		return 0;
	}
	if (sqlite3_prepare_v2(db,
	    "SELECT package,version,revision,state,result_json,duration_s,cpu_s,"
	    "peak_rss_mb,arch,builder_host,finished_at,started_at,published_at"
	    " FROM builds WHERE package=?1"
	    " ORDER BY finished_at IS NULL DESC, finished_at DESC LIMIT ?2",
	    -1, &st, NULL) != SQLITE_OK) {
		*is_error = 1;
		gbuf_adds(text, "queue query failed");
		sqlite3_close(db);
		return 0;
	}
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 2, (int)limit);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && shown < limit) {
		format_build(db, st, text, 0);
		shown++;
	}
	if (shown == 0) {
		gbuf_adds(text, "never built: ");
		gbuf_adds(text, name);
		gbuf_addch(text, '\n');
	} else {
		char tmp[64];
		snprintf(tmp, sizeof tmp, "(showing %ld)\n", shown);
		gbuf_adds(text, tmp);
	}
	sbu_baseline_line(db, name, text);
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "build queue");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_build_log                                                   */
/* ------------------------------------------------------------------ */
static int t_log(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name, *stage;
	int incdis, known = 0, bad = 0;
	long tail;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char logdir[4096];
	(void)err;
	(void)errsz;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	stage = arg_str(args, "stage");
	if (!stage || !stage[0])
		stage = "both";
	if (strcmp(stage, "build") != 0 && strcmp(stage, "install") != 0 &&
	    strcmp(stage, "both") != 0)
		return -1;
	tail = clamp_long(arg_long(args, "tail_bytes", 8000, &bad), 1, 65536);
	if (bad)
		return -1;
	incdis = arg_bool(args, "include_disabled", 0);
	if (incdis < 0)
		return -1;
	if (recipe_is_disabled(name, &known) && !incdis) {
		gbuf_adds(text, "package ");
		gbuf_adds(text, name);
		gbuf_adds(text, " is disabled (build queries exclude it;"
		                 " rerun with include_disabled=true)\n");
		prov(text, "recipes");
		return text->failed ? -1 : 0;
	}
	db = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		return 0;
	}
	if (sqlite3_prepare_v2(db,
	    "SELECT log_dir FROM builds WHERE package=?1"
	    " ORDER BY finished_at IS NULL DESC, finished_at DESC LIMIT 1",
	    -1, &st, NULL) != SQLITE_OK) {
		*is_error = 1;
		gbuf_adds(text, "queue query failed");
		sqlite3_close(db);
		return 0;
	}
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) != SQLITE_ROW ||
	    sqlite3_column_type(st, 0) == SQLITE_NULL) {
		gbuf_adds(text, "no retained log recorded for ");
		gbuf_adds(text, name);
		gbuf_addch(text, '\n');
		sqlite3_finalize(st);
		sqlite3_close(db);
		prov(text, "build queue");
		return text->failed ? -1 : 0;
	}
	snprintf(logdir, sizeof logdir, "%s",
	         (const char *)sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
	sqlite3_close(db);
	{
		const char *stages[2];
		int nst = 0, k;
		if (strcmp(stage, "build") == 0 || strcmp(stage, "both") == 0)
			stages[nst++] = "build.log";
		if (strcmp(stage, "install") == 0 || strcmp(stage, "both") == 0)
			stages[nst++] = "install.log";
		for (k = 0; k < nst; k++) {
			char path[8192];
			gbuf content;
			size_t total = 0;
			int truncated = 0;
			char head[256];
			if (snprintf(path, sizeof path, "%s/%s", logdir, stages[k]) >= (int)sizeof path) {
				gbuf_adds(text, "[path too long]\n");
				continue;
			}
			gbuf_init(&content);
			if (read_file_cap(path, (size_t)tail, &content, &total, &truncated) != 0) {
				gbuf_adds(text, "[");
				gbuf_adds(text, path);
				gbuf_adds(text, ": not retained]\n");
				gbuf_free(&content);
				continue;
			}
			if (truncated)
				snprintf(head, sizeof head, "[%s: showing last %zu of %zu bytes]\n",
				         path, content.len, total);
			else
				snprintf(head, sizeof head, "[%s: %zu bytes]\n", path, total);
			gbuf_adds(text, head);
			gbuf_addn(text, content.p ? content.p : "", content.len);
			if (content.len && content.p[content.len - 1] != '\n')
				gbuf_addch(text, '\n');
			gbuf_free(&content);
		}
	}
	prov(text, "retained logs");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_artifacts                                                   */
/* ------------------------------------------------------------------ */
static int t_artifacts(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name;
	size_t nlen;
	DIR *d;
	struct dirent *e;
	int staged = 0, stale = 0;
	char dbpath[4096];
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	(void)err;
	(void)errsz;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	nlen = strlen(name);
	gbuf_adds(text, "staged:\n");
	d = opendir(path_incoming());
	if (!d) {
		gbuf_adds(text, "(incoming directory unavailable)\n");
	} else {
		while ((e = readdir(d)) != NULL) {
			const char *dn = e->d_name;
			size_t dl;
			int is_live = 0, is_stale = 0;
			if (dn[0] == '.' || strncmp(dn, name, nlen) != 0 || dn[nlen] != '-')
				continue;
			dl = strlen(dn);
			/* NB: readdir names carry no trailing slash. */
			if (dl >= 25 && strcmp(dn + dl - 25, "-ready.superseded-rebuilt") == 0)
				is_stale = 1;
			else if (dl >= 6 && strcmp(dn + dl - 6, "-ready") == 0)
				is_live = 1;
			else if (dl >= 10 && strcmp(dn + dl - 10, "-published") == 0)
				continue; /* history; repository section is authoritative */
			else
				continue;
			if (is_live) {
				char sub[8192];
				DIR *sd;
				struct dirent *se;
				int listed = 0;
				staged++;
				gbuf_adds(text, dn);
				gbuf_adds(text, " [");
				if (snprintf(sub, sizeof sub, "%s/%s", path_incoming(), dn) < (int)sizeof sub &&
				    (sd = opendir(sub)) != NULL) {
					while ((se = readdir(sd)) != NULL) {
						size_t fl = strlen(se->d_name);
						if (fl < 5 || strcmp(se->d_name + fl - 4, ".apk") != 0)
							continue;
						if (listed++)
							gbuf_adds(text, ", ");
						gbuf_adds(text, se->d_name);
					}
					closedir(sd);
				}
				if (!listed)
					gbuf_adds(text, "(empty)");
				gbuf_adds(text, "]\n");
			} else if (is_stale) {
				if (stale < 10) {
					gbuf_adds(text, "(superseded) ");
					gbuf_adds(text, dn);
					gbuf_addch(text, '\n');
				}
				stale++;
			}
		}
		closedir(d);
		if (!staged)
			gbuf_adds(text, "(none staged)\n");
		else if (stale > 10) {
			char tmp[64];
			snprintf(tmp, sizeof tmp, "(+%d more superseded)\n", stale - 10);
			gbuf_adds(text, tmp);
		}
	}
	gbuf_adds(text, "repository:\n");
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db", path_repo_dir()) < (int)sizeof dbpath &&
	    (db = db_open_ro(dbpath, dberr, sizeof dberr)) != NULL) {
		if (sqlite3_prepare_v2(db,
		    "SELECT nvr, published_at, is_newest FROM packages WHERE name=?1"
		    " ORDER BY is_newest DESC, nvr", -1, &st, NULL) == SQLITE_OK) {
			int rows = 0;
			sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
			while (sqlite3_step(st) == SQLITE_ROW && rows < 50) {
				sb_cell(text, st, 0);
				if (sqlite3_column_type(st, 2) != SQLITE_NULL &&
				    sqlite3_column_int(st, 2))
					gbuf_adds(text, " [newest]");
				gbuf_adds(text, " (published ");
				sb_cell(text, st, 1);
				gbuf_adds(text, ")\n");
				rows++;
			}
			if (!rows)
				gbuf_adds(text, "(none published)\n");
		} else {
			gbuf_adds(text, "(repository query failed)\n");
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	} else {
		gbuf_adds(text, "(repository unavailable)\n");
	}
	prov(text, "incoming, repository.db");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* build_queue                                                         */
/* ------------------------------------------------------------------ */
static int t_queue(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	long limit;
	int bad = 0;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	long shown = 0;
	int rc;
	(void)err;
	(void)errsz;
	(void)is_error;
	limit = clamp_long(arg_long(args, "limit", 50, &bad), 1, 200);
	if (bad)
		return -1;
	db = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		return 0;
	}
	if (sqlite3_prepare_v2(db,
	    "SELECT package,version,revision,state,queued_at,started_at"
	    " FROM builds WHERE finished_at IS NULL"
	    " ORDER BY queued_at LIMIT ?1", -1, &st, NULL) != SQLITE_OK) {
		*is_error = 1;
		gbuf_adds(text, "queue query failed");
		sqlite3_close(db);
		return 0;
	}
	sqlite3_bind_int(st, 1, (int)limit);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && shown < limit) {
		sb_cell(text, st, 0);
		gbuf_addch(text, ' ');
		sb_cell(text, st, 1);
		gbuf_adds(text, "-r");
		sb_cell(text, st, 2);
		gbuf_addch(text, ' ');
		sb_cell(text, st, 3);
		gbuf_adds(text, " queued ");
		sb_cell(text, st, 4);
		gbuf_addch(text, '\n');
		shown++;
	}
	if (shown == 0)
		gbuf_adds(text, "queue empty\n");
	else {
		char tmp[64];
		snprintf(tmp, sizeof tmp, "(%ld active)\n", shown);
		gbuf_adds(text, tmp);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "build queue");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_identity                                                    */
/* ------------------------------------------------------------------ */
static void passwd_lookup(const char *path, const char *name, int is_group, gbuf *out) {
	FILE *f;
	char line[1024];
	int found = 0;
	f = fopen(path, "r");
	if (!f) {
		gbuf_adds(out, "(unavailable: ");
		gbuf_adds(out, path);
		gbuf_adds(out, ")\n");
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char *cols[8];
	 int ncol = 0;
		char *p = line;
		size_t L = strlen(line);
		while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
			line[--L] = 0;
		if (!L || line[0] == '#')
			continue;
		while (ncol < 8) {
			cols[ncol++] = p;
			p = strchr(p, ':');
			if (!p)
				break;
			*p++ = 0;
		}
		if (ncol < 3 || strcmp(cols[0], name) != 0)
			continue;
		found = 1;
		if (!is_group && ncol >= 7) {
			gbuf_adds(out, cols[0]);
			gbuf_adds(out, " uid=");
			gbuf_adds(out, cols[2]);
			gbuf_adds(out, " gid=");
			gbuf_adds(out, cols[3]);
			gbuf_adds(out, " home=");
			gbuf_adds(out, cols[5]);
			gbuf_adds(out, " shell=");
			gbuf_adds(out, cols[6]);
			gbuf_addch(out, '\n');
		} else if (is_group && ncol >= 3) {
			gbuf_adds(out, cols[0]);
			gbuf_adds(out, " gid=");
			gbuf_adds(out, cols[2]);
			if (ncol >= 4 && cols[3][0]) {
				gbuf_adds(out, " members=");
				gbuf_adds(out, cols[3]);
			}
			gbuf_addch(out, '\n');
		} else {
			gbuf_adds(out, line);
			gbuf_addch(out, '\n');
		}
	}
	fclose(f);
	if (!found) {
		gbuf_adds(out, name);
		gbuf_adds(out, ": absent\n");
	}
}

#define IDENT_MAX 64
#define IDENT_LEN 128

/* Identity names owned by a package: the 2nd whitespace-separated
 * field of `user`/`group` fragment lines only. `dir`/`file` stanzas
 * name paths in their second field (/var/lib/...), which are not
 * identities - reading those as names once printed phantom
 * "/var/lib/dashboard: absent" rows for a healthy package. The
 * package name itself is NOT an identity either; looking live
 * accounts up by package name is how pkg-a once reported "absent"
 * for a healthy pkguser. */
static int frag_idents(const char *fragp, size_t fraglen,
                       char out[][IDENT_LEN], int max) {
	int n = 0;
	const char *p = fragp, *end = fragp + fraglen;
	while (p < end && n < max) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *eol = nl ? nl : end;
		const char *s = p, *f0, *ns;
		size_t f0len;
		while (s < eol && (*s == ' ' || *s == '\t' || *s == '\r'))
			s++;
		if (s >= eol || *s == '#')
			goto next_frag_line;
		f0 = s;
		while (s < eol && *s != ' ' && *s != '\t' && *s != '\r')
			s++;
		f0len = (size_t)(s - f0);
		if ((f0len != 4 || memcmp(f0, "user", 4) != 0) &&
		    (f0len != 5 || memcmp(f0, "group", 5) != 0))
			goto next_frag_line;
		while (s < eol && (*s == ' ' || *s == '\t'))
			s++;
		ns = s;
		while (s < eol && *s != ' ' && *s != '\t' && *s != '\r')
			s++;
		if (s > ns && (size_t)(s - ns) < IDENT_LEN) {
			int k, dup = 0;
			for (k = 0; k < n; k++) {
				if (strlen(out[k]) == (size_t)(s - ns) &&
				    memcmp(out[k], ns, (size_t)(s - ns)) == 0) {
					dup = 1;
					break;
				}
			}
			if (!dup) {
				memcpy(out[n], ns, (size_t)(s - ns));
				out[n][s - ns] = 0;
				n++;
			}
		}
	next_frag_line:
		if (!nl)
			break;
		p = nl + 1;
	}
	return n;
}

static int ident_match(char idents[][IDENT_LEN], int n,
                       const char *s, size_t sl) {
	int k;
	for (k = 0; k < n; k++) {
		if (strlen(idents[k]) == sl && memcmp(idents[k], s, sl) == 0)
			return 1;
	}
	return 0;
}

static int t_identity(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *name;
	char fragpath[4096];
	char dbpath[4096];
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	gbuf frag;
	size_t total = 0;
	int truncated = 0;
	char idents[IDENT_MAX][IDENT_LEN];
	int nidents = 0, k;
	(void)err;
	(void)errsz;
	(void)is_error;
	name = arg_str(args, "name");
	if (!name || !name[0])
		return -1;
	gbuf_adds(text, "fragment:\n");
	if (snprintf(fragpath, sizeof fragpath, "%s/%s/files/accounts.d/%s",
	             path_recipes(), name, name) >= (int)sizeof fragpath) {
		gbuf_adds(text, "(path too long)\n");
	} else {
		gbuf_init(&frag);
		if (read_file_cap(fragpath, 4096, &frag, &total, &truncated) != 0) {
			gbuf_adds(text, "(no fragment declared)\n");
		} else {
			gbuf_addn(text, frag.p ? frag.p : "", frag.len);
			if (frag.len && frag.p[frag.len - 1] != '\n')
				gbuf_addch(text, '\n');
			if (truncated)
				gbuf_adds(text, "(fragment truncated at 4KiB)\n");
			if (frag.p)
				nidents = frag_idents(frag.p, frag.len, idents, IDENT_MAX);
		}
		gbuf_free(&frag);
	}
	gbuf_adds(text, "registry (accounts.tsv):\n");
	{
		FILE *f = fopen(path_tsv(), "r");
		char line[1024];
		int found = 0;
		if (!f) {
			gbuf_adds(text, "(registry unavailable)\n");
		} else {
			while (fgets(line, sizeof line, f)) {
				char *t1, *nm;
				size_t L = strlen(line);
				while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
					line[--L] = 0;
				if (!L || line[0] == '#')
					continue;
				/* accounts.tsv is `name\tkind\t...` (see its
				 * header row): the identity is the FIRST
				 * field, unlike accounts.d fragments where
				 * the kind comes first. */
				t1 = strchr(line, '\t');
				if (!t1)
					continue;
				*t1 = 0;
				nm = line;
				if (strcmp(nm, name) == 0 ||
				    ident_match(idents, nidents, nm, strlen(nm))) {
					*t1 = '\t';
					gbuf_adds(text, line);
					gbuf_addch(text, '\n');
					found = 1;
				}
			}
			fclose(f);
			if (!found)
				gbuf_adds(text, "(no registry rows)\n");
		}
	}
	gbuf_adds(text, "reservations (repository.db):\n");
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db", path_repo_dir()) < (int)sizeof dbpath &&
	    (db = db_open_ro(dbpath, dberr, sizeof dberr)) != NULL) {
		if (sqlite3_prepare_v2(db,
		    "SELECT kind,name,numeric_id,owning_package,last_nvr,status"
		    " FROM reservations WHERE owning_package=?1 OR name=?1 ORDER BY kind",
		    -1, &st, NULL) == SQLITE_OK) {
			int rows = 0;
			sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
			while (sqlite3_step(st) == SQLITE_ROW) {
				sb_cell(text, st, 0);
				gbuf_addch(text, ' ');
				sb_cell(text, st, 1);
				gbuf_addch(text, ' ');
				sb_cell(text, st, 2);
				gbuf_adds(text, " owned by ");
				sb_cell(text, st, 3);
				gbuf_addch(text, ' ');
				sb_cell(text, st, 4);
				gbuf_addch(text, ' ');
				sb_cell(text, st, 5);
				gbuf_addch(text, '\n');
				rows++;
			}
			if (!rows)
				gbuf_adds(text, "(none registered)\n");
		} else {
			gbuf_adds(text, "(reservations query failed)\n");
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	} else {
		gbuf_adds(text, "(repository unavailable)\n");
	}
	gbuf_adds(text, "live user:\n");
	if (nidents > 0) {
		for (k = 0; k < nidents; k++)
			passwd_lookup(path_passwd(), idents[k], 0, text);
	} else {
		passwd_lookup(path_passwd(), name, 0, text);
	}
	gbuf_adds(text, "live group:\n");
	if (nidents > 0) {
		for (k = 0; k < nidents; k++)
			passwd_lookup(path_group(), idents[k], 1, text);
	} else {
		passwd_lookup(path_group(), name, 1, text);
	}
	prov(text, "accounts.d, accounts.tsv, repository.db, live accounts");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package evidence journal (notes.db; Egg-local, append-only)         */
/*                                                                     */
/* The journal is the MCP's own database: engineering notes and policy */
/* evidence pinned to the revisions they were verified against.        */
/* Keeper and Homer are only backup destinations, fed by SQLite-safe   */
/* snapshots (`sqlite3 notes.db .backup snapshot.db`, which uses the   */
/* online backup API) - never by copying the live file, and never via  */
/* Git, which remains source-code version control only. The schema is  */
/* owned here (CREATE TABLE IF NOT EXISTS on open): notes.db must     */
/* never be carried in /recipes except blank and empty.                */
/*                                                                     */
/* `recorder` is MCP clientInfo metadata: client-reported, unverified, */
/* displayed as-is, never trusted for decisions (this server makes     */
/* none). Real attribution lives in `author` ($SBM_AUTHOR).            */
/* ------------------------------------------------------------------ */

static char journal_recorder[128];

void sbm_set_recorder(const char *recorder) {
	if (!recorder)
		recorder = "";
	snprintf(journal_recorder, sizeof journal_recorder, "%s", recorder);
}

#define NOTE_TITLE_CAP 256
#define NOTE_BODY_CAP 4096
#define EVIDENCE_CAP 8192
#define CREDIT_CAP 127

/* Journal note kinds: finding/decision/issue describe the world and its
 * open questions; success/failure/lesson describe judged outcomes and
 * what to carry forward. success = went well and why (repeat it);
 * failure = went badly, the wrong choice or assumption and what it
 * cost (record the error, not blame); lesson = prescriptive
 * carry-forward distilled from either. Only issue counts as open. */
static int kind_valid(const char *k) {
	return strcmp(k, "finding") == 0 || strcmp(k, "decision") == 0 ||
	       strcmp(k, "issue") == 0 || strcmp(k, "success") == 0 ||
	       strcmp(k, "failure") == 0 || strcmp(k, "lesson") == 0;
}

static const char *policy_checks[] = {
	"unix-identity",
	"payload-layout",
	"revision-discipline",
	"dependency-closure",
	"signing-acceptance",
	NULL
};

static int check_valid(const char *c) {
	int i;
	for (i = 0; policy_checks[i]; i++) {
		if (strcmp(policy_checks[i], c) == 0)
			return 1;
	}
	return 0;
}

static int result_valid(const char *r) {
	return strcmp(r, "PASS") == 0 || strcmp(r, "FAIL") == 0;
}

static void resolve_credit(jv *args, char *author, size_t asz) {
	const char *a = arg_str(args, "author");
	if (!a || !a[0])
		a = env_or("SBM_AUTHOR", "");
	if (!a || !a[0])
		a = "unspecified";
	snprintf(author, asz, "%s", a);
}

#define SESSION_CAP 127
#define PROV_HOST_CAP 255
#define PROV_USER_CAP 255
#define PROV_CWD_CAP 1023
#define PROV_AGENT_CAP 63

/* Execution provenance for a journal write. Client-supplied
 * arguments win and mark origin "client"; gaps fall back to the
 * server fill (SBM_* overrides, else live values, origin
 * "server"). Returns -1 on an overlong value (writers refuse);
 * unknown fields stay empty so storage binds NULL. */
static int prov_take(jv *args, prov_t *p) {
	const char *v;
	int client = 0;
	prov_server_fill(p);
	if (!args)
		return 0;
	v = arg_str(args, "session_id");
	if (v && v[0]) {
		if (strlen(v) > SESSION_CAP)
			return -1;
		snprintf(p->session, sizeof p->session, "%s", v);
		client = 1;
	}
	v = arg_str(args, "hostname");
	if (v && v[0]) {
		if (strlen(v) > PROV_HOST_CAP)
			return -1;
		snprintf(p->host, sizeof p->host, "%s", v);
		client = 1;
	}
	v = arg_str(args, "username");
	if (v && v[0]) {
		if (strlen(v) > PROV_USER_CAP)
			return -1;
		snprintf(p->user, sizeof p->user, "%s", v);
		client = 1;
	}
	v = arg_str(args, "cwd");
	if (v && v[0]) {
		if (strlen(v) > PROV_CWD_CAP)
			return -1;
		snprintf(p->cwd, sizeof p->cwd, "%s", v);
		client = 1;
	}
	v = arg_str(args, "agent");
	if (v && v[0]) {
		if (strlen(v) > PROV_AGENT_CAP)
			return -1;
		snprintf(p->agent, sizeof p->agent, "%s", v);
		client = 1;
	}
	if (client)
		snprintf(p->origin, sizeof p->origin, "client");
	return 0;
}

static void utc_now(char out[21]) {
	time_t t = time(NULL);
	struct tm tm;
	sb_gmtime(&t, &tm);
	strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Print s indented by two spaces, preserving inner newlines. */
static void indented(gbuf *b, const char *s) {
	gbuf_adds(b, "  ");
	for (; *s; s++) {
		gbuf_addch(b, *s);
		if (*s == '\n' && s[1])
			gbuf_adds(b, "  ");
	}
	if (b->len && b->p[b->len - 1] != '\n')
		gbuf_addch(b, '\n');
}

/* sha256 hex of one file, computed in-process. The previous
 * /bin/sha256sum frontend silently failed on hosts without
 * coreutils (macOS has no sha256sum), pinning every freshness
 * check to a digest that could never move. */
static int sha256_hex_file(const char *path, char out[65]) {
	if (!path)
		return -1;
	return sb_sha256_file(path, out);
}

/* Recipe commit via read-only git; "" when unavailable (not a checkout,
 * git missing, unknown package). Digests remain the freshness key. */
static void git_recipe_commit(const char *pkg, char out[41]) {
	char *av[9];
	gbuf so, se;
	int trunc = 0, rc, i;
	char ebuf[128];
	out[0] = 0;
	gbuf_init(&so);
	gbuf_init(&se);
	av[0] = "git";
	av[1] = "-C";
	av[2] = (char *)path_recipes();
	av[3] = "log";
	av[4] = "-1";
	av[5] = "--format=%H";
	av[6] = "--";
	av[7] = (char *)pkg;
	av[8] = NULL;
	rc = run_capture(av, &so, &se, &trunc, ebuf, sizeof ebuf);
	if (rc == 0 && !so.failed && so.len >= 41) {
		for (i = 0; i < 40; i++) {
			char c = so.p[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
			      (c >= 'A' && c <= 'F')))
				break;
		}
		if (i == 40 && so.p[40] == '\n') {
			memcpy(out, so.p, 40);
			out[40] = 0;
		}
	}
	gbuf_free(&so);
	gbuf_free(&se);
}

typedef struct {
	char recipe_commit[41];
	char recipe_digest[65];
	char registry_digest[150];
	char artifact_nvr[256];
} pins_t;

/* Live pins for freshness comparison. Missing sources pin as "" (or
 * "-" for artifacts): stable across reads, so absence alone never
 * flips a record to STALE - only observed change does. */
static void current_pins(const char *pkg, pins_t *pins) {
	char rpath[4096], hex[65], frag[4096];
	char tsvhex[65], fraghex[65];
	char dbpath[4096], dberr[128];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	memset(pins, 0, sizeof *pins);
	git_recipe_commit(pkg, pins->recipe_commit);
	if (snprintf(rpath, sizeof rpath, "%s/%s/recipe.sh",
	             path_recipes(), pkg) < (int)sizeof rpath &&
	    sha256_hex_file(rpath, hex) == 0)
		memcpy(pins->recipe_digest, hex, 65);
	if (sha256_hex_file(path_tsv(), tsvhex) == 0) {
		if (snprintf(frag, sizeof frag, "%s/%s/files/accounts.d/%s",
		             path_recipes(), pkg, pkg) < (int)sizeof frag &&
		    sha256_hex_file(frag, fraghex) == 0)
			snprintf(pins->registry_digest, sizeof pins->registry_digest,
			         "%s:%s", tsvhex, fraghex);
		else
			snprintf(pins->registry_digest, sizeof pins->registry_digest,
			         "%s:-", tsvhex);
	}
	snprintf(pins->artifact_nvr, sizeof pins->artifact_nvr, "-");
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db",
	             path_repo_dir()) < (int)sizeof dbpath &&
	    (db = db_open_ro(dbpath, dberr, sizeof dberr)) != NULL) {
		if (sqlite3_prepare_v2(db,
		    "SELECT nvr FROM packages WHERE name=?1 AND is_newest=1",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW) {
				const char *nvr = (const char *)sqlite3_column_text(st, 0);
				if (nvr)
					snprintf(pins->artifact_nvr,
					         sizeof pins->artifact_nvr, "%s", nvr);
			}
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	}
}

/* Latest record id for (package, check), or 0 when none. */
static long latest_record(sqlite3 *db, const char *pkg, const char *check) {
	sqlite3_stmt *st = NULL;
	long id = 0;
	if (sqlite3_prepare_v2(db,
	    "SELECT id FROM policy_records WHERE package=?1 AND check_name=?2"
	    " ORDER BY id DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, check, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW)
		id = (long)sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return id;
}

/* Compare stored pins against live; appends moved-pin reasons.
 * Returns 1 when current, 0 when stale. */
static int pins_current(const pins_t *cur, const char *rcommit,
                        const char *rdigest, const char *gdigest,
                        const char *nvr, gbuf *why) {
	int ok = 1;
	int first = 1;
	if (!rcommit)
		rcommit = "";
	if (!rdigest)
		rdigest = "";
	if (!gdigest)
		gdigest = "";
	if (!nvr)
		nvr = "-";
	if (strcmp(rdigest, cur->recipe_digest) != 0) {
		gbuf_adds(why, "recipe moved");
		first = 0;
		ok = 0;
	}
	if (strcmp(gdigest, cur->registry_digest) != 0) {
		if (!first)
			gbuf_adds(why, "; ");
		gbuf_adds(why, "registry moved");
		first = 0;
		ok = 0;
	}
	if (strcmp(nvr, cur->artifact_nvr) != 0) {
		if (!first)
			gbuf_adds(why, "; ");
		gbuf_adds(why, "artifact moved");
		ok = 0;
	}
	(void)rcommit;
	(void)first;
	return ok;
}

/* ------------------------------------------------------------------ */
/* package_notes_add                                                   */
/* ------------------------------------------------------------------ */
static int t_notes_add(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg, *kind, *title, *body, *aa;
	char author[CREDIT_CAP + 1], dberr[256], now[21], rcommit[41];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	prov_t pv;
	int rc, bi;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	kind = arg_str(args, "kind");
	body = arg_str(args, "body");
	title = arg_str(args, "title");
	aa = arg_str(args, "author");
	if (!pkg || !pkg[0] || !kind || !kind[0] || !body)
		return -1;
	if (!title)
		title = "";
	if (!kind_valid(kind))
		return -1;
	if (strlen(title) > NOTE_TITLE_CAP || strlen(body) > NOTE_BODY_CAP)
		return -1;
	if (aa && strlen(aa) > CREDIT_CAP)
		return -1;
	if (prov_take(args, &pv) != 0)
		return -1;
	resolve_credit(args, author, sizeof author);
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	utc_now(now);
	git_recipe_commit(pkg, rcommit);
	rc = sqlite3_prepare_v2(db,
	    "INSERT INTO notes(package,kind,title,body,author,recorder,recipe_commit,created_at,"
	    "session_id,hostname,username,cwd,agent,ctx_origin)"
	    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)", -1, &st, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 4, body, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 5, author, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 6, journal_recorder, -1, SQLITE_TRANSIENT);
		if (rcommit[0])
			sqlite3_bind_text(st, 7, rcommit, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(st, 7);
		sqlite3_bind_text(st, 8, now, -1, SQLITE_TRANSIENT);
		bi = 9;
		prov_bind(st, &bi, &pv);
		rc = sqlite3_step(st);
	}
	if (rc == SQLITE_DONE) {
		char tmp[128];
		snprintf(tmp, sizeof tmp, "recorded note %lld for ",
		         (long long)sqlite3_last_insert_rowid(db));
		gbuf_adds(text, tmp);
		gbuf_adds(text, pkg);
		gbuf_adds(text, " [");
		gbuf_adds(text, kind);
		gbuf_adds(text, "] by ");
		gbuf_adds(text, author);
		gbuf_addch(text, '\n');
	} else {
		*is_error = 1;
		gbuf_adds(text, "journal write failed\n");
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_notes_get                                                   */
/* ------------------------------------------------------------------ */
static int t_notes_get(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg, *kind;
	long limit;
	int bad = 0;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int rc, shown = 0;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	kind = arg_str(args, "kind");
	if (!pkg || !pkg[0])
		return -1;
	if (kind && !kind_valid(kind))
		return -1;
	limit = clamp_long(arg_long(args, "limit", 20, &bad), 1, 100);
	if (bad)
		return -1;
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	gbuf_adds(text, "notes for ");
	gbuf_adds(text, pkg);
	gbuf_adds(text, ":\n");
	if (sqlite3_prepare_v2(db,
	    kind ? "SELECT id,kind,title,body,author,recorder,recipe_commit,created_at"
	           " FROM notes WHERE package=?1 AND kind=?2 ORDER BY id DESC LIMIT ?3"
	         : "SELECT id,kind,title,body,author,recorder,recipe_commit,created_at"
	           " FROM notes WHERE package=?1 ORDER BY id DESC LIMIT ?2",
	    -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		if (kind) {
			sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(st, 3, (int)limit);
		} else {
			sqlite3_bind_int(st, 2, (int)limit);
		}
		while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
			char tmp[64];
			const char *ttl = (const char *)sqlite3_column_text(st, 2);
			const char *bdy = (const char *)sqlite3_column_text(st, 3);
			snprintf(tmp, sizeof tmp, "[%lld] ",
			         (long long)sqlite3_column_int64(st, 0));
			gbuf_adds(text, tmp);
			sb_cell(text, st, 1);
			gbuf_addch(text, ' ');
			sb_cell(text, st, 7);
			gbuf_adds(text, " by ");
			sb_cell(text, st, 4);
			if (sqlite3_column_type(st, 5) != SQLITE_NULL &&
			    sqlite3_column_text(st, 5)[0]) {
				gbuf_adds(text, " recorder=");
				sb_cell(text, st, 5);
			}
			if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
				gbuf_adds(text, " commit=");
				sb_cell(text, st, 6);
			}
			gbuf_addch(text, '\n');
			if (ttl && ttl[0]) {
				gbuf_adds(text, "  title: ");
				gbuf_adds(text, ttl);
				gbuf_addch(text, '\n');
			}
			if (bdy)
				indented(text, bdy);
			shown++;
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	if (!shown)
		gbuf_adds(text, "(no notes recorded)\n");
	else {
		char tmp[64];
		snprintf(tmp, sizeof tmp, "(showing %d)\n", shown);
		gbuf_adds(text, tmp);
	}
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_policy_record                                               */
/* ------------------------------------------------------------------ */
static int t_policy_record(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg, *check, *result, *aa;
	jv *ev;
	char *evcopy = NULL;
	char author[CREDIT_CAP + 1], dberr[256], now[21];
	pins_t pins;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	prov_t pv;
	int rc, bi;
	long sid;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	check = arg_str(args, "check");
	result = arg_str(args, "result");
	aa = arg_str(args, "author");
	if (!pkg || !pkg[0] || !check || !check[0] || !result)
		return -1;
	if (!check_valid(check) || !result_valid(result))
		return -1;
	if (aa && strlen(aa) > CREDIT_CAP)
		return -1;
	if (prov_take(args, &pv) != 0)
		return -1;
	ev = args ? jv_get(args, "evidence") : NULL;
	if (ev) {
		if (ev->t != 5 || !ev->raw || ev->rawlen == 0 ||
		    ev->rawlen > EVIDENCE_CAP)
			return -1;
		evcopy = malloc(ev->rawlen + 1);
		if (!evcopy)
			return -1;
		memcpy(evcopy, ev->raw, ev->rawlen);
		evcopy[ev->rawlen] = 0;
	}
	/* A PASS without evidence is an attestation of confidence, not a
	 * verified check: refuse it. (Presence is enforceable; truth is
	 * the author's - the author field says who claims it.) A FAIL
	 * needs no evidence object: the failure itself is the record,
	 * with detail living in build logs and notes. */
	if (strcmp(result, "PASS") == 0 && !evcopy)
		return -1;
	resolve_credit(args, author, sizeof author);
	current_pins(pkg, &pins);
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		free(evcopy);
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	sid = latest_record(db, pkg, check);
	utc_now(now);
	rc = sqlite3_prepare_v2(db,
	    "INSERT INTO policy_records(package,check_name,result,recipe_commit,"
	    "recipe_digest,registry_digest,artifact_nvr,evidence_json,author,"
	    "recorder,created_at,supersedes_id,"
	    "session_id,hostname,username,cwd,agent,ctx_origin)"
	    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
	    "?13,?14,?15,?16,?17,?18)", -1, &st, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, check, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, result, -1, SQLITE_TRANSIENT);
		if (pins.recipe_commit[0])
			sqlite3_bind_text(st, 4, pins.recipe_commit, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(st, 4);
		sqlite3_bind_text(st, 5, pins.recipe_digest, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 6, pins.registry_digest, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 7, pins.artifact_nvr, -1, SQLITE_TRANSIENT);
		if (evcopy)
			sqlite3_bind_text(st, 8, evcopy, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(st, 8);
		sqlite3_bind_text(st, 9, author, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 10, journal_recorder, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 11, now, -1, SQLITE_TRANSIENT);
		if (sid > 0)
			sqlite3_bind_int64(st, 12, (long long)sid);
		else
			sqlite3_bind_null(st, 12);
		bi = 13;
		prov_bind(st, &bi, &pv);
		rc = sqlite3_step(st);
	}
	free(evcopy);
	if (rc == SQLITE_DONE) {
		char tmp[192];
		if (sid > 0)
			snprintf(tmp, sizeof tmp, "recorded %s=%s for %s (record %lld, supersedes %ld)",
			         check, result, pkg,
			         (long long)sqlite3_last_insert_rowid(db), sid);
		else
			snprintf(tmp, sizeof tmp, "recorded %s=%s for %s (record %lld, first record)",
			         check, result, pkg,
			         (long long)sqlite3_last_insert_rowid(db));
		gbuf_adds(text, tmp);
		gbuf_addch(text, '\n');
	} else {
		*is_error = 1;
		gbuf_adds(text, "journal write failed\n");
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* Print one check's latest record with freshness, for status/readiness. */
static void print_check(sqlite3 *db, const char *pkg, const char *check,
                        const pins_t *cur, gbuf *text, int *have,
                        int *current, int *stale) {
	sqlite3_stmt *st = NULL;
	*have = 0;
	*current = 0;
	*stale = 0;
	if (sqlite3_prepare_v2(db,
	    "SELECT id,result,author,recorder,recipe_commit,recipe_digest,"
	    "registry_digest,artifact_nvr,evidence_json,created_at"
	    " FROM policy_records WHERE package=?1 AND check_name=?2"
	    " ORDER BY id DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, check, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		gbuf why;
		const char *rcommit = (const char *)sqlite3_column_text(st, 4);
		const char *rdigest = (const char *)sqlite3_column_text(st, 5);
		const char *gdigest = (const char *)sqlite3_column_text(st, 6);
		const char *nvr = (const char *)sqlite3_column_text(st, 7);
		char tmp[256];
		*have = 1;
		gbuf_init(&why);
		if (pins_current(cur, rcommit, rdigest, gdigest, nvr, &why)) {
			*current = 1;
			snprintf(tmp, sizeof tmp, "%s: %s CURRENT (record %lld, %s by %s",
			         check, sqlite3_column_text(st, 1),
			         (long long)sqlite3_column_int64(st, 0),
			         sqlite3_column_text(st, 9),
			         sqlite3_column_text(st, 2));
		} else {
			*stale = 1;
			snprintf(tmp, sizeof tmp, "%s: %s STALE (record %lld; %s",
			         check, sqlite3_column_text(st, 1),
			         (long long)sqlite3_column_int64(st, 0),
			         why.p ? why.p : "pins moved");
		}
		gbuf_adds(text, tmp);
		if (rcommit && rcommit[0]) {
			char sh[9];
			memcpy(sh, rcommit, 7);
			sh[7] = 0;
			gbuf_adds(text, ", commit ");
			gbuf_adds(text, sh);
		}
		gbuf_adds(text, ")\n");
		if (!*current && !*stale) {
			gbuf_free(&why);
			sqlite3_finalize(st);
			return;
		}
		if (sqlite3_column_type(st, 8) != SQLITE_NULL) {
			gbuf_adds(text, "  evidence: ");
			sb_cell(text, st, 8);
			gbuf_addch(text, '\n');
		}
		gbuf_free(&why);
	}
	sqlite3_finalize(st);
}

/* ------------------------------------------------------------------ */
/* package_policy_status                                               */
/* ------------------------------------------------------------------ */
static int t_policy_status(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg;
	char dberr[256];
	sqlite3 *db = NULL;
	pins_t pins;
	int i;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	if (!pkg || !pkg[0])
		return -1;
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	current_pins(pkg, &pins);
	gbuf_adds(text, "policy status for ");
	gbuf_adds(text, pkg);
	gbuf_adds(text, ":\n");
	for (i = 0; policy_checks[i]; i++) {
		int have = 0, current = 0, stale = 0;
		print_check(db, pkg, policy_checks[i], &pins, text,
		            &have, &current, &stale);
		if (!have) {
			gbuf_adds(text, policy_checks[i]);
			gbuf_adds(text, ": UNVERIFIED (no record)\n");
		}
	}
	sqlite3_close(db);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* First line of a note body, bounded for readiness auto-recall.
 * Predictable small addition: at most 160 chars, cut at the first
 * newline, overlong lines gain "...". NULL-safe. Full bodies stay
 * behind package_notes_get (selective pull). */
static void body_first_line(gbuf *text, const unsigned char *body) {
	size_t i = 0;
	if (!body || !body[0]) {
		gbuf_adds(text, "(no body)");
		return;
	}
	while (body[i] && body[i] != '\n' && i < 160) {
		gbuf_addch(text, (char)body[i]);
		i++;
	}
	if (body[i] && body[i] != '\n')
		gbuf_adds(text, "...");
}

/* ------------------------------------------------------------------ */
/* package_readiness                                                   */
/* ------------------------------------------------------------------ */
static int t_readiness(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	pins_t pins;
	int i, first;
	long nnotes = 0, nissues = 0;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	if (!pkg || !pkg[0])
		return -1;
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	current_pins(pkg, &pins);
	gbuf_adds(text, "readiness for ");
	gbuf_adds(text, pkg);
	gbuf_adds(text, ":\nverified (current):\n");
	first = 1;
	for (i = 0; policy_checks[i]; i++) {
		int have = 0, current = 0, stale = 0;
		gbuf row;
		gbuf_init(&row);
		print_check(db, pkg, policy_checks[i], &pins, &row,
		            &have, &current, &stale);
		if (have && current) {
			gbuf_adds(text, "  ");
			gbuf_adds(text, row.p ? row.p : "");
			first = 0;
		}
		gbuf_free(&row);
	}
	if (first)
		gbuf_adds(text, "  (none)\n");
	gbuf_adds(text, "stale (needs revalidation):\n");
	first = 1;
	for (i = 0; policy_checks[i]; i++) {
		int have = 0, current = 0, stale = 0;
		gbuf row;
		gbuf_init(&row);
		print_check(db, pkg, policy_checks[i], &pins, &row,
		            &have, &current, &stale);
		if (have && stale) {
			gbuf_adds(text, "  ");
			gbuf_adds(text, row.p ? row.p : "");
			first = 0;
		}
		gbuf_free(&row);
	}
	if (first)
		gbuf_adds(text, "  (none)\n");
	gbuf_adds(text, "unverified:\n  ");
	first = 1;
	for (i = 0; policy_checks[i]; i++) {
		int have = 0, current = 0, stale = 0;
		gbuf row;
		gbuf_init(&row);
		print_check(db, pkg, policy_checks[i], &pins, &row,
		            &have, &current, &stale);
		gbuf_free(&row);
		if (!have) {
			if (!first)
				gbuf_adds(text, ", ");
			gbuf_adds(text, policy_checks[i]);
			first = 0;
		}
	}
	if (first)
		gbuf_adds(text, "(none - every check has evidence)");
	gbuf_addch(text, '\n');
	if (sqlite3_prepare_v2(db,
	    "SELECT COUNT(*),SUM(CASE WHEN kind='issue' THEN 1 ELSE 0 END)"
	    " FROM notes WHERE package=?1", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW) {
			nnotes = (long)sqlite3_column_int64(st, 0);
			nissues = (long)sqlite3_column_int64(st, 1);
		}
	}
	sqlite3_finalize(st);
	st = NULL;
	/* Auto-recall: inline the compact chain so one call carries the
	 * constraint (e.g. musl calibration blocked, dashboard INSTALL vs
	 * refusal). Bounded: newest 3 only, first line of each body, full
	 * bodies via package_notes_get. */
	{
		sqlite3_stmt *rs = NULL;
		int shown = 0;
		gbuf_adds(text, "recent:\n");
		if (sqlite3_prepare_v2(db,
		    "SELECT id,kind,title,body,author,created_at"
		    " FROM notes WHERE package=?1 ORDER BY id DESC LIMIT 3",
		    -1, &rs, NULL) == SQLITE_OK) {
			sqlite3_bind_text(rs, 1, pkg, -1, SQLITE_TRANSIENT);
			while (sqlite3_step(rs) == SQLITE_ROW) {
				const unsigned char *ttl =
				    sqlite3_column_text(rs, 2);
				const unsigned char *bdy =
				    sqlite3_column_text(rs, 3);
				char tmp[64];
				snprintf(tmp, sizeof tmp, "  [%lld] ",
				         (long long)sqlite3_column_int64(rs, 0));
				gbuf_adds(text, tmp);
				sb_cell(text, rs, 1);
				gbuf_addch(text, ' ');
				sb_cell(text, rs, 5);
				gbuf_adds(text, " by ");
				sb_cell(text, rs, 4);
				gbuf_addch(text, '\n');
				if (ttl && ttl[0]) {
					gbuf_adds(text, "    title: ");
					gbuf_adds(text, (const char *)ttl);
					gbuf_addch(text, '\n');
				}
				gbuf_adds(text, "    body: ");
				body_first_line(text, bdy);
				gbuf_addch(text, '\n');
				shown++;
			}
		}
		sqlite3_finalize(rs);
		if (!shown)
			gbuf_adds(text, "  (no notes recorded)\n");
		else if (nnotes > shown) {
			char tmp[64];
			snprintf(tmp, sizeof tmp,
			         "  (... %ld more; full bodies via"
			         " package_notes_get)\n", nnotes - shown);
			gbuf_adds(text, tmp);
		} else
			gbuf_adds(text, "  (full bodies via package_notes_get)\n");
	}
	sqlite3_close(db);
	{
		char tmp[128];
		snprintf(tmp, sizeof tmp, "notes: %ld (%ld open issue%s)\n",
		         nnotes, nissues, nissues == 1 ? "" : "s");
		gbuf_adds(text, tmp);
	}
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

#define REASON_CAP 512

/* Newest published NVR for a package, or "" when unpublished. */
static void repo_newest_nvr(const char *pkg, char *out, size_t outsz) {
	char dbpath[4096], dberr[128];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	out[0] = 0;
	if (snprintf(dbpath, sizeof dbpath, "%s/repository.db",
	             path_repo_dir()) >= (int)sizeof dbpath)
		return;
	db = db_open_ro(dbpath, dberr, sizeof dberr);
	if (!db)
		return;
	if (sqlite3_prepare_v2(db,
	    "SELECT nvr FROM packages WHERE name=?1 AND is_newest=1 LIMIT 1",
	    -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *nvr = (const char *)sqlite3_column_text(st, 0);
			if (nvr)
				snprintf(out, outsz, "%s", nvr);
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
}

/* First line of captured output, bounded. */
static void output_first(gbuf *src, char *out, size_t outsz) {
	size_t i = 0, j = 0;
	while (i < src->len && j + 1 < outsz && i < 200) {
		if (src->p[i] == '\n')
			break;
		out[j++] = src->p[i++];
	}
	out[j] = 0;
	if (!j)
		snprintf(out, outsz, "(no output)");
}

/* Append-only journal write shared by the action tools. Every state
 * change the MCP performs records why (the caller's reason) alongside
 * what happened. Returns the note id, or -1 with dberr set. */
static long journal_note(const char *pkg, const char *kind,
                         const char *title, const char *body,
                         const char *author, char *dberr, size_t dberrsz) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char now[21];
	prov_t pv;
	int rc, bi;
	long id = -1;
	snprintf(dberr, dberrsz, "journal unavailable");
	/* Auto-notes carry server provenance (no caller context). */
	prov_take(NULL, &pv);
	db = db_open_notes(dberr, dberrsz);
	if (!db)
		return -1;
	utc_now(now);
	rc = sqlite3_prepare_v2(db,
	    "INSERT INTO notes(package,kind,title,body,author,recorder,created_at,"
	    "session_id,hostname,username,cwd,agent,ctx_origin)"
	    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)", -1, &st, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 4, body, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 5, author, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 6, journal_recorder, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 7, now, -1, SQLITE_TRANSIENT);
		bi = 8;
		prov_bind(st, &bi, &pv);
		rc = sqlite3_step(st);
	}
	if (rc == SQLITE_DONE)
		id = (long)sqlite3_last_insert_rowid(db);
	else
		snprintf(dberr, dberrsz, "journal write failed");
	sqlite3_finalize(st);
	sqlite3_close(db);
	return id;
}

/* ------------------------------------------------------------------ */
/* build_request: queue a build through the daemon intake              */
/*                                                                     */
/* Runs `saphira-build scan` (argv, no shell) and reports what queued  */
/* for the requested package. The daemon executes the build detached;  */
/* the MCP never runs buildpkg itself and never blocks on the build.   */
/* Scan is tree-wide by daemon design (it only queues genuinely        */
/* changed work); the response names what moved for this package.      */
/* sudo-gated steps stay human: the response carries the exact ask.    */
/* ------------------------------------------------------------------ */
static int t_build_request(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg, *reason, *aa;
	char author[CREDIT_CAP + 1], dberr[256], ebuf[256], first[220];
	char nvr[280], newest[256], notetitle[300], notebody[1024];
	recipe_t *pkgs = NULL;
	size_t n = 0;
	char *av[3];
	gbuf so, se;
	int trunc = 0, rc;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	long before = 0, queued = 0;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	reason = arg_str(args, "reason");
	aa = arg_str(args, "author");
	if (!pkg || !pkg[0] || !reason || !reason[0])
		return -1;
	if (strlen(reason) > REASON_CAP)
		return -1;
	if (aa && strlen(aa) > CREDIT_CAP)
		return -1;
	resolve_credit(args, author, sizeof author);
	if (recipe_collect(&pkgs, &n) != 0) {
		*is_error = 1;
		gbuf_adds(text, "cannot list recipes");
		prov(text, "recipes");
		return 0;
	}
	{
		recipe_t *r = find_recipe(pkgs, n, pkg);
		if (!r) {
			*is_error = 1;
			gbuf_adds(text, "unknown package: ");
			gbuf_adds(text, pkg);
			gbuf_addch(text, '\n');
			recipe_free_all(pkgs, n);
			prov(text, "recipes");
			return 0;
		}
		if (r->disabled) {
			*is_error = 1;
			gbuf_adds(text, "package ");
			gbuf_adds(text, pkg);
			gbuf_adds(text, " is disabled; builds never queue for disabled recipes\n");
			recipe_free_all(pkgs, n);
			prov(text, "recipes");
			return 0;
		}
		nvr_rest(r->ver[0] ? r->ver : "?", r->rel[0] ? r->rel : "?",
		         nvr, sizeof nvr);
		{
			char full[280];
			snprintf(full, sizeof full, "%s-%s", r->name, nvr);
			recipe_free_all(pkgs, n);
			repo_newest_nvr(pkg, newest, sizeof newest);
			if (newest[0] && strcmp(newest, full) == 0) {
				gbuf_adds(text, "already published (");
				gbuf_adds(text, full);
				gbuf_adds(text, "); nothing queued\n");
				prov(text, "recipes, repository.db");
				return text->failed ? -1 : 0;
			}
		}
	}
	db = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (db) {
		if (sqlite3_prepare_v2(db,
		    "SELECT COALESCE(MAX(id),0) FROM builds WHERE package=?1",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW)
				before = (long)sqlite3_column_int64(st, 0);
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	}
	gbuf_init(&so);
	gbuf_init(&se);
	av[0] = "saphira-build";
	av[1] = "scan";
	av[2] = NULL;
	rc = run_capture(av, &so, &se, &trunc, ebuf, sizeof ebuf);
	if (rc != 0 || so.failed) {
		output_first(&se, first, sizeof first);
		*is_error = 1;
		if (rc != 0) {
			gbuf_adds(text, "build scan failed: ");
			gbuf_adds(text, first);
		} else {
			gbuf_adds(text, "build scan output error");
		}
		gbuf_addch(text, '\n');
		gbuf_free(&so);
		gbuf_free(&se);
		prov(text, "saphira-build scan");
		return 0;
	}
	gbuf_free(&so);
	gbuf_free(&se);
	gbuf_adds(text, "build request for ");
	gbuf_adds(text, pkg);
	gbuf_adds(text, " (scan exit 0):\n  reason: ");
	gbuf_adds(text, reason);
	gbuf_addch(text, '\n');
	db = db_open_ro(path_queue_db(), dberr, sizeof dberr);
	if (db) {
		if (sqlite3_prepare_v2(db,
		    "SELECT id,state FROM builds WHERE package=?1 AND id>?2 ORDER BY id",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(st, 2, (long long)before);
			while (sqlite3_step(st) == SQLITE_ROW) {
				char tmp[128];
				snprintf(tmp, sizeof tmp, "  queued row %lld: %s\n",
				         (long long)sqlite3_column_int64(st, 0),
				         sqlite3_column_text(st, 1));
				gbuf_adds(text, tmp);
				queued++;
			}
		}
		sqlite3_finalize(st);
		sqlite3_close(db);
	}
	if (!queued)
		gbuf_adds(text, "  no new rows for this package (already queued, failed, or unchanged)\n");
	snprintf(notetitle, sizeof notetitle, "build requested: %s", pkg);
	snprintf(notebody, sizeof notebody, "%s\nscan queued %ld new row(s)", reason, queued);
	{
		long nid = journal_note(pkg, "decision", notetitle, notebody, author,
		                        dberr, sizeof dberr);
		char tmp[128];
		if (nid > 0)
			snprintf(tmp, sizeof tmp, "note %ld recorded: build requested\n", nid);
		else
			snprintf(tmp, sizeof tmp, "WARNING: journal write failed (%s)\n", dberr);
		gbuf_adds(text, tmp);
	}
	gbuf_adds(text, "the builder daemon executes detached; follow with build_queue / package_build_status\n");
	gbuf_adds(text, "when status shows AWAITING SIGNING, ask the operator to run `sudo sign-apk-repo ");
	gbuf_adds(text, pkg);
	gbuf_adds(text, "`\n");
	prov(text, "saphira-build scan, build queue, notes.db");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* package_bump: revision bump with guards, rationale journaled        */
/*                                                                     */
/* Runs `bumppkg --yes <pkg>` (argv, no shell). bumppkg owns every     */
/* pkgrel transition and refuses with reasons (never-published,        */
/* 24h anti-loop, disabled); refusals surface as isError, never as     */
/* silent success. A successful bump records the caller's reason in    */
/* the journal: revision changes must remember why.                    */
/* ------------------------------------------------------------------ */
static int t_package_bump(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *pkg, *reason, *aa;
	char author[CREDIT_CAP + 1], dberr[256], ebuf[256], first[220];
	char notetitle[300], notebody[1024];
	recipe_t *pkgs = NULL;
	size_t n = 0;
	char *av[4];
	gbuf so, se;
	int trunc = 0, rc;
	(void)err;
	(void)errsz;
	pkg = arg_str(args, "package");
	reason = arg_str(args, "reason");
	aa = arg_str(args, "author");
	if (!pkg || !pkg[0] || !reason || !reason[0])
		return -1;
	if (strlen(reason) > REASON_CAP)
		return -1;
	if (aa && strlen(aa) > CREDIT_CAP)
		return -1;
	resolve_credit(args, author, sizeof author);
	if (recipe_collect(&pkgs, &n) != 0) {
		*is_error = 1;
		gbuf_adds(text, "cannot list recipes");
		prov(text, "recipes");
		return 0;
	}
	{
		recipe_t *r = find_recipe(pkgs, n, pkg);
		if (!r) {
			*is_error = 1;
			gbuf_adds(text, "unknown package: ");
			gbuf_adds(text, pkg);
			gbuf_addch(text, '\n');
			recipe_free_all(pkgs, n);
			prov(text, "recipes");
			return 0;
		}
		if (r->disabled) {
			*is_error = 1;
			gbuf_adds(text, "package ");
			gbuf_adds(text, pkg);
			gbuf_adds(text, " is disabled; revisions never bump for disabled recipes\n");
			recipe_free_all(pkgs, n);
			prov(text, "recipes");
			return 0;
		}
		recipe_free_all(pkgs, n);
	}
	gbuf_init(&so);
	gbuf_init(&se);
	av[0] = "bumppkg";
	av[1] = "--yes";
	av[2] = (char *)pkg;
	av[3] = NULL;
	rc = run_capture(av, &so, &se, &trunc, ebuf, sizeof ebuf);
	output_first(rc == 0 ? &so : &se, first, sizeof first);
	if (rc != 0) {
		*is_error = 1;
		gbuf_adds(text, "bump refused or failed: ");
		gbuf_adds(text, first);
		gbuf_addch(text, '\n');
		gbuf_free(&so);
		gbuf_free(&se);
		prov(text, "bumppkg");
		return 0;
	}
	gbuf_adds(text, "package_bump for ");
	gbuf_adds(text, pkg);
	gbuf_adds(text, ":\n  reason: ");
	gbuf_adds(text, reason);
	gbuf_addch(text, '\n');
	gbuf_adds(text, "  ");
	gbuf_adds(text, first);
	gbuf_addch(text, '\n');
	gbuf_free(&so);
	gbuf_free(&se);
	snprintf(notetitle, sizeof notetitle, "revision bumped: %s", pkg);
	snprintf(notebody, sizeof notebody, "%s\nbumppkg: %s", reason, first);
	{
		long nid = journal_note(pkg, "decision", notetitle, notebody, author,
		                        dberr, sizeof dberr);
		char tmp[128];
		if (nid > 0)
			snprintf(tmp, sizeof tmp, "note %ld recorded: revision bumped\n", nid);
		else
			snprintf(tmp, sizeof tmp, "WARNING: bump succeeded but journal write failed (%s)\n", dberr);
		gbuf_adds(text, tmp);
	}
	prov(text, "bumppkg, notes.db");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* checkpoint_save / checkpoint_resume                                 */
/* ------------------------------------------------------------------ */
#define CHECKPOINT_LABEL_CAP 64
#define CHECKPOINT_BODY_CAP 16384
#define CHECKPOINT_PROJECT_CAP 64
#define CHECKPOINT_WORKSTREAM_CAP 64

/* A checkpoint is a pre-restart write meant to be consumed post-restart:
 * "resume from checkpoint <id|label>" must restore exactly what the
 * previous session knew. The body carries the narrative (state, next
 * action, blockers); label groups retries of one line of work; commits
 * pin the tree. 16 KiB body cap: a proper checkpoint does not fit in a
 * 4 KiB note. Commits are 40-hex when supplied, informational otherwise. */
static int is_hex40(const char *s) {
	size_t i;
	if (strlen(s) != 40)
		return 0;
	for (i = 0; i < 40; i++) {
		if (!isxdigit((unsigned char)s[i]))
			return 0;
	}
	return 1;
}

static int t_checkpoint_save(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *label, *title, *body, *pkg, *aa, *src, *rcp, *proj, *ws;
	char author[CREDIT_CAP + 1], dberr[256], now[21];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	prov_t pv;
	int rc, bi;
	(void)err;
	(void)errsz;
	label = arg_str(args, "label");
	body = arg_str(args, "body");
	title = arg_str(args, "title");
	pkg = arg_str(args, "package");
	aa = arg_str(args, "author");
	src = arg_str(args, "src_commit");
	rcp = arg_str(args, "recipe_commit");
	proj = arg_str(args, "project");
	ws = arg_str(args, "workstream");
	if (!label || !label[0] || !body)
		return -1;
	if (!title)
		title = "";
	if (!pkg)
		pkg = "";
	if (!proj)
		proj = "";
	if (!ws)
		ws = "";
	if (strlen(label) > CHECKPOINT_LABEL_CAP ||
	    strlen(title) > NOTE_TITLE_CAP || strlen(body) > CHECKPOINT_BODY_CAP)
		return -1;
	if (strlen(proj) > CHECKPOINT_PROJECT_CAP ||
	    strlen(ws) > CHECKPOINT_WORKSTREAM_CAP)
		return -1;
	if (aa && strlen(aa) > CREDIT_CAP)
		return -1;
	if (src && src[0] && !is_hex40(src))
		return -1;
	if (rcp && rcp[0] && !is_hex40(rcp))
		return -1;
	if (prov_take(args, &pv) != 0)
		return -1;
	resolve_credit(args, author, sizeof author);
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	utc_now(now);
	rc = sqlite3_prepare_v2(db,
	    "INSERT INTO checkpoints(label,package,title,body,author,recorder,"
	    "src_commit,recipe_commit,created_at,status,project,workstream,"
	    "session_id,hostname,username,cwd,agent,ctx_origin)"
	    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,'active',?10,?11,"
	    "?12,?13,?14,?15,?16,?17)", -1, &st, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_text(st, 1, label, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, pkg, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 4, body, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 5, author, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 6, journal_recorder, -1, SQLITE_TRANSIENT);
		if (src && src[0])
			sqlite3_bind_text(st, 7, src, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(st, 7);
		if (rcp && rcp[0])
			sqlite3_bind_text(st, 8, rcp, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(st, 8);
		sqlite3_bind_text(st, 9, now, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 10, proj, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 11, ws, -1, SQLITE_TRANSIENT);
		bi = 12;
		prov_bind(st, &bi, &pv);
		rc = sqlite3_step(st);
	}
	if (rc == SQLITE_DONE) {
		char tmp[192];
		snprintf(tmp, sizeof tmp, "recorded checkpoint %lld (label '%s')\n",
		         (long long)sqlite3_last_insert_rowid(db), label);
		gbuf_adds(text, tmp);
	} else {
		*is_error = 1;
		gbuf_adds(text, "journal write failed\n");
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* Print one checkpoint row in full: consumption, not preview. A resumed
 * session needs the whole narrative, so the body is never truncated
 * (the 256 KiB result cap bounds abuse, not this reader). */
static void print_checkpoint(gbuf *text, sqlite3_stmt *st) {
	char tmp[128];
	snprintf(tmp, sizeof tmp, "checkpoint [%lld] (label '",
	         (long long)sqlite3_column_int64(st, 0));
	gbuf_adds(text, tmp);
	sb_cell(text, st, 1);
	gbuf_adds(text, "') ");
	sb_cell(text, st, 8);
	gbuf_adds(text, " by ");
	sb_cell(text, st, 5);
	if (sqlite3_column_type(st, 6) != SQLITE_NULL &&
	    sqlite3_column_text(st, 6)[0]) {
		gbuf_adds(text, " recorder=");
		sb_cell(text, st, 6);
	}
	gbuf_addch(text, '\n');
	/* Columns 15-20 are v0.10.0 execution provenance (session,
	 * host, user, cwd, agent, origin). Printed when anything is
	 * known; absent entirely for pre-provenance rows. Never
	 * fabricate: NULL fields are omitted, not defaulted. */
	if (sqlite3_column_count(st) > 20) {
		const unsigned char *h = sqlite3_column_text(st, 16);
		const unsigned char *u = sqlite3_column_text(st, 17);
		const unsigned char *d = sqlite3_column_text(st, 18);
		const unsigned char *o = sqlite3_column_text(st, 20);
		const unsigned char *s = sqlite3_column_text(st, 15);
		const unsigned char *a = sqlite3_column_text(st, 19);
		if ((h && h[0]) || (u && u[0]) || (d && d[0])) {
			gbuf_adds(text, "  where: ");
			if (h && h[0]) {
				gbuf_adds(text, (const char *)h);
			} else {
				gbuf_adds(text, "?");
			}
			if (u && u[0]) {
				gbuf_adds(text, " as ");
				gbuf_adds(text, (const char *)u);
			}
			if (d && d[0]) {
				gbuf_adds(text, " in ");
				gbuf_adds(text, (const char *)d);
			}
			if ((o && o[0]) || (s && s[0]) || (a && a[0])) {
				gbuf_adds(text, " (");
				if (o && o[0]) {
					gbuf_adds(text, (const char *)o);
				} else {
					gbuf_adds(text, "?");
				}
				if (s && s[0]) {
					gbuf_adds(text, ", session ");
					gbuf_adds(text, (const char *)s);
				}
				if (a && a[0]) {
					gbuf_adds(text, ", agent ");
					gbuf_adds(text, (const char *)a);
				}
				gbuf_adds(text, ")");
			}
			gbuf_addch(text, '\n');
		}
	}
	if (sqlite3_column_type(st, 2) != SQLITE_NULL &&
	    sqlite3_column_text(st, 2)[0]) {
		gbuf_adds(text, "  scope: ");
		sb_cell(text, st, 2);
		gbuf_addch(text, '\n');
	} else {
		gbuf_adds(text, "  scope: (session-global)\n");
	}
	if (sqlite3_column_type(st, 3) != SQLITE_NULL &&
	    sqlite3_column_text(st, 3)[0]) {
		gbuf_adds(text, "  title: ");
		gbuf_adds(text, (const char *)sqlite3_column_text(st, 3));
		gbuf_addch(text, '\n');
	}
	/* Columns 10-14 are v2 (0.7.0). A legacy row predating the
	 * register reads as active with no project/workstream: work
	 * written before statuses existed is unfinished by definition. */
	if (sqlite3_column_count(st) > 10 &&
	    sqlite3_column_type(st, 10) != SQLITE_NULL) {
		gbuf_adds(text, "  status: ");
		sb_cell(text, st, 10);
		gbuf_addch(text, '\n');
	} else {
		gbuf_adds(text, "  status: active\n");
	}
	if (sqlite3_column_count(st) > 11 &&
	    sqlite3_column_type(st, 11) != SQLITE_NULL &&
	    sqlite3_column_text(st, 11)[0]) {
		gbuf_adds(text, "  project: ");
		sb_cell(text, st, 11);
		gbuf_addch(text, '\n');
	}
	if (sqlite3_column_count(st) > 12 &&
	    sqlite3_column_type(st, 12) != SQLITE_NULL &&
	    sqlite3_column_text(st, 12)[0]) {
		gbuf_adds(text, "  workstream: ");
		sb_cell(text, st, 12);
		gbuf_addch(text, '\n');
	}
	if (sqlite3_column_count(st) > 13 &&
	    sqlite3_column_type(st, 13) != SQLITE_NULL &&
	    sqlite3_column_text(st, 13)[0]) {
		gbuf_adds(text, "  closed: ");
		sb_cell(text, st, 13);
		if (sqlite3_column_count(st) > 14 &&
		    sqlite3_column_type(st, 14) != SQLITE_NULL &&
		    sqlite3_column_text(st, 14)[0]) {
			gbuf_adds(text, " by ");
			sb_cell(text, st, 14);
		}
		gbuf_addch(text, '\n');
	}
	if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
		gbuf_adds(text, "  src_commit: ");
		sb_cell(text, st, 7);
		gbuf_addch(text, '\n');
	}
	if (sqlite3_column_type(st, 9) != SQLITE_NULL) {
		gbuf_adds(text, "  recipe_commit: ");
		sb_cell(text, st, 9);
		gbuf_addch(text, '\n');
	}
	gbuf_adds(text, "  body:\n");
	indented(text, (const char *)sqlite3_column_text(st, 4));
}

static int t_checkpoint_resume(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *label;
	long id;
	int bad = 0;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int rc, found = 0;
	(void)err;
	(void)errsz;
	label = arg_str(args, "label");
	id = arg_long(args, "id", -1, &bad);
	if (bad)
		return -1;
	if (id != -1 && id <= 0)
		return -1;
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	/* Columns: 0 id, 1 label, 2 package, 3 title, 4 body, 5 author,
	 * 6 recorder, 7 src_commit, 8 created_at, 9 recipe_commit,
	 * 10 status, 11 project, 12 workstream, 13 closed_at,
	 * 14 closed_by, 15 session_id, 16 hostname, 17 username,
	 * 18 cwd, 19 agent, 20 ctx_origin. Resume never filters on
	 * status: a closed checkpoint stays consumable by id
	 * (history is never deleted); the active register lives in
	 * checkpoint_list. */
	if (id != -1) {
		rc = sqlite3_prepare_v2(db,
		    "SELECT id,label,package,title,body,author,recorder,"
		    "src_commit,created_at,recipe_commit,"
		    "status,project,workstream,closed_at,closed_by,"
		    "session_id,hostname,username,cwd,agent,ctx_origin"
		    " FROM checkpoints WHERE id=?1", -1, &st, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, (long long)id);
			if (sqlite3_step(st) == SQLITE_ROW)
				found = 1;
		}
		if (!found) {
			char tmp[64];
			snprintf(tmp, sizeof tmp, "no such checkpoint %ld\n", id);
			*is_error = 1;
			gbuf_adds(text, tmp);
		}
	} else if (label && label[0]) {
		rc = sqlite3_prepare_v2(db,
		    "SELECT id,label,package,title,body,author,recorder,"
		    "src_commit,created_at,recipe_commit,"
		    "status,project,workstream,closed_at,closed_by,"
		    "session_id,hostname,username,cwd,agent,ctx_origin"
		    " FROM checkpoints WHERE label=?1 ORDER BY id DESC LIMIT 1",
		    -1, &st, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(st, 1, label, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW)
				found = 1;
		}
		if (!found) {
			*is_error = 1;
			gbuf_adds(text, "no checkpoints for label '");
			gbuf_adds(text, label);
			gbuf_adds(text, "'\n");
		}
	} else {
		rc = sqlite3_prepare_v2(db,
		    "SELECT id,label,package,title,body,author,recorder,"
		    "src_commit,created_at,recipe_commit,"
		    "status,project,workstream,closed_at,closed_by,"
		    "session_id,hostname,username,cwd,agent,ctx_origin"
		    " FROM checkpoints ORDER BY id DESC LIMIT 1",
		    -1, &st, NULL);
		if (rc == SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW)
				found = 1;
		}
		if (!found)
			gbuf_adds(text, "(no checkpoints recorded)\n");
	}
	if (found)
		print_checkpoint(text, st);
	sqlite3_finalize(st);
	sqlite3_close(db);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* checkpoint_list: the active register. Preview, not consumption:
 * one line per checkpoint (id, status, label, project/workstream,
 * title, author, age), newest first, plus the matching total so a
 * capped page never hides the size of the backlog. Closed work is
 * hidden by default; status=closed|all (or include_closed) reveals
 * it. Closing never deletes: checkpoint_resume by id still serves
 * closed rows in full. */
#define CHECKPOINT_LIST_DEFAULT 20
#define CHECKPOINT_LIST_MAX 100

static int t_checkpoint_list(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *status, *proj, *ws, *label;
	long limit, total = 0;
	int bad = 0, inc, explicit_status, n = 0, nw;
	char dberr[256], where[448], tmp[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int all, closed_only;
	(void)err;
	(void)errsz;
	explicit_status = args && jv_get(args, "status") ? 1 : 0;
	status = arg_str(args, "status");
	if (!status)
		status = "active";
	if (strcmp(status, "active") != 0 && strcmp(status, "closed") != 0 &&
	    strcmp(status, "all") != 0)
		return -1;
	inc = arg_bool(args, "include_closed", 0);
	if (inc < 0)
		return -1;
	if (inc && explicit_status && strcmp(status, "active") == 0)
		return -1; /* contradictory filters fail closed */
	if (inc && !explicit_status)
		status = "all";
	proj = arg_str(args, "project");
	ws = arg_str(args, "workstream");
	label = arg_str(args, "label");
	if (proj && strlen(proj) > CHECKPOINT_PROJECT_CAP)
		return -1;
	if (ws && strlen(ws) > CHECKPOINT_WORKSTREAM_CAP)
		return -1;
	if (label && strlen(label) > CHECKPOINT_LABEL_CAP)
		return -1;
	limit = arg_long(args, "limit", CHECKPOINT_LIST_DEFAULT, &bad);
	if (bad)
		return -1;
	limit = clamp_long(limit, 1, CHECKPOINT_LIST_MAX);
	all = strcmp(status, "all") == 0;
	closed_only = strcmp(status, "closed") == 0;
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	snprintf(where, sizeof where, "%s%s%s%s",
	    all ? "1=1" : (closed_only ? "status='closed'" : "status='active'"),
	    proj && proj[0] ? " AND project=?1" : "",
	    ws && ws[0] ? " AND workstream=?2" : "",
	    label && label[0] ? " AND label=?3" : "");
	nw = snprintf(tmp, sizeof tmp,
	    "SELECT COUNT(*) FROM checkpoints WHERE %s", where);
	if (nw < 0 || nw >= (int)sizeof tmp)
		return -1; /* filter text cannot overflow: fail loudly */
	if (sqlite3_prepare_v2(db, tmp, -1, &st, NULL) == SQLITE_OK) {
		int bi = 1;
		if (proj && proj[0])
			sqlite3_bind_text(st, bi++, proj, -1, SQLITE_TRANSIENT);
		if (ws && ws[0])
			sqlite3_bind_text(st, bi++, ws, -1, SQLITE_TRANSIENT);
		if (label && label[0])
			sqlite3_bind_text(st, bi++, label, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW)
			total = (long)sqlite3_column_int64(st, 0);
	}
	sqlite3_finalize(st);
	st = NULL;
	nw = snprintf(tmp, sizeof tmp,
	    "SELECT id,label,project,workstream,title,author,status,created_at"
	    " FROM checkpoints WHERE %s ORDER BY id DESC LIMIT %ld",
	    where, limit);
	if (nw < 0 || nw >= (int)sizeof tmp)
		return -1;
	if (sqlite3_prepare_v2(db, tmp, -1, &st, NULL) == SQLITE_OK) {
		int bi = 1;
		if (proj && proj[0])
			sqlite3_bind_text(st, bi++, proj, -1, SQLITE_TRANSIENT);
		if (ws && ws[0])
			sqlite3_bind_text(st, bi++, ws, -1, SQLITE_TRANSIENT);
		if (label && label[0])
			sqlite3_bind_text(st, bi++, label, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(st) == SQLITE_ROW) {
			const unsigned char *l, *p, *w, *t, *a, *s, *c;
			n++;
			gbuf_adds(text, "  [");
			sb_cell(text, st, 0);
			gbuf_adds(text, "]");
			s = sqlite3_column_text(st, 6);
			if (s && strcmp((const char *)s, "active") != 0) {
				gbuf_adds(text, " (");
				gbuf_adds(text, (const char *)s);
				gbuf_adds(text, ")");
			}
			l = sqlite3_column_text(st, 1);
			gbuf_adds(text, " label '");
			gbuf_adds(text, l ? (const char *)l : "");
			gbuf_adds(text, "'");
			p = sqlite3_column_text(st, 2);
			if (p && p[0]) {
				gbuf_adds(text, " project '");
				gbuf_adds(text, (const char *)p);
				gbuf_adds(text, "'");
			}
			w = sqlite3_column_text(st, 3);
			if (w && w[0]) {
				gbuf_adds(text, " workstream '");
				gbuf_adds(text, (const char *)w);
				gbuf_adds(text, "'");
			}
			t = sqlite3_column_text(st, 4);
			if (t && t[0]) {
				gbuf_adds(text, " '");
				gbuf_adds(text, (const char *)t);
				gbuf_adds(text, "'");
			}
			a = sqlite3_column_text(st, 5);
			gbuf_adds(text, " by ");
			gbuf_adds(text, a ? (const char *)a : "-");
			c = sqlite3_column_text(st, 7);
			gbuf_adds(text, " at ");
			gbuf_adds(text, c ? (const char *)c : "-");
			gbuf_addch(text, '\n');
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	if (n == 0) {
		if (all)
			gbuf_adds(text, "(no checkpoints recorded)\n");
		else {
			snprintf(tmp, sizeof tmp, "(no %s checkpoints)\n", status);
			gbuf_adds(text, tmp);
		}
	} else {
		snprintf(tmp, sizeof tmp, "%s checkpoints: %ld total (showing %d)\n",
		    all ? "all" : status, total, n);
		/* Header belongs first: prepend by rebuilding. The page
		 * is tiny (limit <= 100 short rows), so one copy is fine. */
		gbuf head;
		gbuf_init(&head);
		gbuf_adds(&head, tmp);
		if (text->p)
			gbuf_addn(&head, text->p, text->len);
		gbuf_free(text);
		*text = head;
	}
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* checkpoint_close: retire finished work from the active register.
 * Sets status (never deletes: the row stays resumable by id).
 * Idempotent: closing a closed checkpoint reports its state. */
static int t_checkpoint_close(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	long id;
	int bad = 0;
	char author[CREDIT_CAP + 1], closer[CREDIT_CAP + 1];
	char dberr[256], now[21], tmp[192];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int rc, found = 0;
	(void)err;
	(void)errsz;
	id = arg_long(args, "id", -1, &bad);
	if (bad || id <= 0)
		return -1;
	resolve_credit(args, author, sizeof author);
	snprintf(closer, sizeof closer, "%s", author);
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	rc = sqlite3_prepare_v2(db,
	    "SELECT id,label,status FROM checkpoints WHERE id=?1", -1, &st, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, (long long)id);
		if (sqlite3_step(st) == SQLITE_ROW)
			found = 1;
	}
	if (!found) {
		snprintf(tmp, sizeof tmp, "no such checkpoint %ld\n", id);
		*is_error = 1;
		gbuf_adds(text, tmp);
		sqlite3_finalize(st);
		sqlite3_close(db);
		prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
		return 0;
	}
	{
		const unsigned char *s = sqlite3_column_text(st, 2);
		const unsigned char *l = sqlite3_column_text(st, 1);
		char label[CHECKPOINT_LABEL_CAP + 1];
		int already = s && strcmp((const char *)s, "closed") == 0;
		snprintf(label, sizeof label, "%s", l ? (const char *)l : "");
		sqlite3_finalize(st);
		st = NULL;
		if (already) {
			snprintf(tmp, sizeof tmp,
			    "checkpoint %ld (label '%s') already closed\n", id, label);
			gbuf_adds(text, tmp);
			sqlite3_close(db);
			prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
			return 0;
		}
		utc_now(now);
		rc = sqlite3_prepare_v2(db,
		    "UPDATE checkpoints SET status='closed',closed_at=?1,closed_by=?2"
		    " WHERE id=?3", -1, &st, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(st, 1, now, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 2, closer, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(st, 3, (long long)id);
			rc = sqlite3_step(st);
		}
		sqlite3_finalize(st);
		if (rc == SQLITE_DONE) {
			snprintf(tmp, sizeof tmp,
			    "closed checkpoint %ld (label '%s')\n", id, label);
			gbuf_adds(text, tmp);
		} else {
			*is_error = 1;
			gbuf_adds(text, "journal write failed\n");
		}
	}
	sqlite3_close(db);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* initialize instructions: the checkpoint register at connect time  */
/* ------------------------------------------------------------------ */
#define CHECKPOINT_INSTRUCTIONS_N 5
#define CHECKPOINT_INSTRUCTIONS_TITLE 64

static void instr_title(gbuf *out, const unsigned char *t) {
	size_t n;
	if (!t || !t[0])
		return;
	gbuf_adds(out, " '");
	n = strlen((const char *)t);
	if (n > CHECKPOINT_INSTRUCTIONS_TITLE) {
		gbuf_addn(out, (const char *)t, CHECKPOINT_INSTRUCTIONS_TITLE - 3);
		gbuf_adds(out, "...'");
	} else {
		gbuf_adds(out, (const char *)t);
		gbuf_adds(out, "'");
	}
}

void checkpoint_instructions(gbuf *out) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char ebuf[256], tmp[96];
	long total = 0;
	int n = 0, v2 = 1;
	gbuf_adds(out,
	    "Session checkpoints are a persistent register of unfinished work"
	    " shared by all agents on this host."
	    " checkpoint_list shows the active register (newest first, with a"
	    " total count); checkpoint_resume consumes one by id (a bare number"
	    " like \"3\" means checkpoint id 3); checkpoint_close retires"
	    " finished work without deleting its history. On session start,"
	    " list the active checkpoints and ask the operator whether to"
	    " resume one, start fresh, or close one. Never auto-resume. ");
	/* Read-only: never create or migrate the journal at connect
	 * time. A missing or legacy journal degrades to static
	 * guidance (legacy rows predate status: all count as active). */
	db = db_open_ro(path_notes_db(), ebuf, sizeof ebuf);
	if (!db)
		return;
	if (sqlite3_prepare_v2(db,
	    "SELECT COUNT(*) FROM checkpoints WHERE status='active'",
	    -1, &st, NULL) != SQLITE_OK) {
		sqlite3_finalize(st);
		st = NULL;
		v2 = 0;
		if (sqlite3_prepare_v2(db,
		    "SELECT COUNT(*) FROM checkpoints", -1, &st, NULL) != SQLITE_OK) {
			sqlite3_close(db);
			return;
		}
	}
	if (sqlite3_step(st) == SQLITE_ROW)
		total = (long)sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	st = NULL;
	if (total <= 0) {
		gbuf_adds(out, "No active checkpoints recorded.");
		sqlite3_close(db);
		return;
	}
	snprintf(tmp, sizeof tmp, "Active checkpoints (%ld total): ", total);
	gbuf_adds(out, tmp);
	if (v2) {
		sqlite3_prepare_v2(db,
		    "SELECT id,label,project,workstream,title,"
		    "hostname,username,cwd FROM checkpoints"
		    " WHERE status='active' ORDER BY id DESC LIMIT "
		    "5", -1, &st, NULL);
	} else {
		/* Legacy shape: project slot carries the scope. */
		sqlite3_prepare_v2(db,
		    "SELECT id,label,package,'','',title FROM checkpoints"
		    " ORDER BY id DESC LIMIT 5", -1, &st, NULL);
	}
	if (st) {
		while (sqlite3_step(st) == SQLITE_ROW) {
			const unsigned char *l, *p, *w;
			if (n > 0)
				gbuf_adds(out, "; ");
			snprintf(tmp, sizeof tmp, "[%lld] label '",
			    (long long)sqlite3_column_int64(st, 0));
			gbuf_adds(out, tmp);
			l = sqlite3_column_text(st, 1);
			gbuf_adds(out, l ? (const char *)l : "");
			gbuf_adds(out, "'");
			p = sqlite3_column_text(st, 2);
			if (p && p[0]) {
				gbuf_adds(out, " project '");
				gbuf_adds(out, (const char *)p);
				gbuf_adds(out, "'");
			}
			w = sqlite3_column_text(st, 3);
			if (w && w[0]) {
				gbuf_adds(out, " workstream '");
				gbuf_adds(out, (const char *)w);
				gbuf_adds(out, "'");
			}
			/* Columns 5-7 are v0.10.0 execution provenance;
			 * shown when known so the register names where
			 * each piece of work lives. */
			if (sqlite3_column_count(st) > 7) {
				const unsigned char *h =
				    sqlite3_column_text(st, 5);
				const unsigned char *u =
				    sqlite3_column_text(st, 6);
				const unsigned char *d =
				    sqlite3_column_text(st, 7);
				if ((h && h[0]) || (u && u[0]) ||
				    (d && d[0])) {
					gbuf_adds(out, " where ");
					if (h && h[0])
						gbuf_adds(out,
						    (const char *)h);
					else
						gbuf_adds(out, "?");
					if (u && u[0]) {
						gbuf_adds(out, " as ");
						gbuf_adds(out,
						    (const char *)u);
					}
					if (d && d[0]) {
						gbuf_adds(out, " in ");
						gbuf_adds(out,
						    (const char *)d);
					}
				}
			}
			instr_title(out, sqlite3_column_text(st, 4));
			n++;
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	if (total > n) {
		snprintf(tmp, sizeof tmp,
		    "; and %ld more. checkpoint_list shows the full register.", total - n);
		gbuf_adds(out, tmp);
	} else {
		gbuf_adds(out, ".");
	}
}

/* ------------------------------------------------------------------ */
/* total_recall                                                          */
/* ------------------------------------------------------------------ */
/* Bounded historical retrieval over the journal: FTS5/BM25 ranked
 * query, chronological timeline, and paginated export. This is not
 * perfect memory — it retrieves only what was recorded (notes,
 * checkpoints, policy evidence, tool-run metadata; tool outputs
 * and conversation turns were never captured). Checkpoints restore
 * working state cheaply; total_recall digs deeper only when asked,
 * never automatically. */
#define TR_LIMIT_DFLT 20
#define TR_LIMIT_MAX 100

static long tr_tier_bytes(const char *budget) {
	if (!budget || strcmp(budget, "compact") == 0)
		return 4096;
	if (strcmp(budget, "standard") == 0)
		return 16384;
	if (strcmp(budget, "extended") == 0)
		return 65536;
	if (strcmp(budget, "deep") == 0)
		return 262144;
	return -1;
}

/* Split a query into FTS5 phrase tokens joined by OR. Tokens are
 * double-quoted with embedded quotes doubled (the documented
 * escape); tokens without alphanumerics are dropped. Returns -1
 * when nothing searchable remains. */
static int tr_match_expr(const char *query, char *out, size_t outsz) {
	size_t pos = 0;
	int first = 1, any = 0;
	const char *p = query;
	out[0] = 0;
	while (*p) {
		const char *tok;
		size_t len = 0;
		int alnum = 0;
		size_t i;
		while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
			p++;
		if (!*p)
			break;
		tok = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
			if ((*p >= 'A' && *p <= 'Z') ||
			    (*p >= 'a' && *p <= 'z') ||
			    (*p >= '0' && *p <= '9'))
				alnum = 1;
			p++;
			len++;
		}
		if (!alnum || len == 0 || len > 128)
			continue;
		if (!first) {
			if (pos + 4 >= outsz)
				return -1;
			memcpy(out + pos, " OR ", 4);
			pos += 4;
		}
		first = 0;
		if (pos + 1 >= outsz)
			return -1;
		out[pos++] = '"';
		for (i = 0; i < len; i++) {
			if (tok[i] == '"') {
				if (pos + 2 >= outsz)
					return -1;
				out[pos++] = '"';
				out[pos++] = '"';
			} else {
				if (pos + 1 >= outsz)
					return -1;
				out[pos++] = tok[i];
			}
		}
		if (pos + 1 >= outsz)
			return -1;
		out[pos++] = '"';
		any = 1;
	}
	out[pos] = 0;
	return any ? 0 : -1;
}

/* Escape LIKE metacharacters with backslash. */
static void tr_like_esc(const char *s, char *out, size_t outsz) {
	size_t pos = 0;
	for (; *s && pos + 2 < outsz; s++) {
		if (*s == '%' || *s == '_' || *s == '\\')
			out[pos++] = '\\';
		out[pos++] = *s;
	}
	out[pos] = 0;
}

typedef struct {
	char *session, *package, *label, *kind, *title, *body, *author;
	char *host, *user, *cwd, *agent, *origin, *created, *table;
	long long sid;
	double rank;
	char *snippet;
} tr_row;

static char *tr_dup(const unsigned char *s) {
	const char *t = s ? (const char *)s : "";
	char *d = malloc(strlen(t) + 1);
	if (d)
		memcpy(d, t, strlen(t) + 1);
	return d;
}

/* Free one row's heap strings (the slot itself is caller-owned;
 * never frees r). */
static void tr_free_row(tr_row *r) {
	free(r->session);
	free(r->package);
	free(r->label);
	free(r->kind);
	free(r->title);
	free(r->body);
	free(r->author);
	free(r->host);
	free(r->user);
	free(r->cwd);
	free(r->agent);
	free(r->origin);
	free(r->created);
	free(r->table);
	free(r->snippet);
}

/* Free n rows' strings plus the array itself. */
static void tr_free_rows(tr_row *rows, int n) {
	int i;
	if (!rows)
		return;
	for (i = 0; i < n; i++)
		tr_free_row(&rows[i]);
	free(rows);
}

/* Cursor: timeline/export carry "created_at|table|id", query mode
 * carries "o<offset>". Parse into triple (table validated) or
 * offset; garbage means start over (cursor ignored, never fatal). */
static int tr_parse_cursor(const char *cursor, char *created, size_t csz,
                           char *table, size_t tsz, long long *sid,
                           long *offset) {
	const char *a, *b;
	size_t n;
	*offset = 0;
	if (!cursor || !cursor[0])
		return 0;
	if (cursor[0] == 'o') {
		char *end = NULL;
		long o = strtol(cursor + 1, &end, 10);
		if (end && *end == 0 && o >= 0) {
			*offset = o;
			return 1;
		}
		return 0;
	}
	a = strchr(cursor, '|');
	if (!a)
		return 0;
	b = strchr(a + 1, '|');
	if (!b || strchr(b + 1, '|'))
		return 0;
	n = (size_t)(a - cursor);
	if (n == 0 || n >= csz)
		return 0;
	memcpy(created, cursor, n);
	created[n] = 0;
	n = (size_t)(b - (a + 1));
	if (n == 0 || n >= tsz)
		return 0;
	memcpy(table, a + 1, n);
	table[n] = 0;
	if (strcmp(table, "checkpoints") != 0 &&
	    strcmp(table, "notes") != 0 &&
	    strcmp(table, "policy_records") != 0 &&
	    strcmp(table, "tool_runs") != 0 &&
	    strcmp(table, "session_events") != 0)
		return 0;
	{
		char *end = NULL;
		long long id = strtoll(b + 1, &end, 10);
		if (!end || *end != 0 || id <= 0)
			return 0;
		*sid = id;
	}
	return 2;
}
/* Fetch one row's display fields by (table, id) into r (all fields
 * heap-duplicated; missing row leaves r zeroed). Returns 1 when
 * found, 0 otherwise. */
static int tr_fetch(sqlite3 *db, const char *table, long long id,
                    tr_row *r) {
	sqlite3_stmt *st = NULL;
	const char *sql = NULL;
	memset(r, 0, sizeof *r);
	if (strcmp(table, "checkpoints") == 0)
		sql = "SELECT session_id,package,label,'checkpoint',title,"
		      "body,author,hostname,username,cwd,agent,ctx_origin,"
		      "created_at FROM checkpoints WHERE id=?1";
	else if (strcmp(table, "notes") == 0)
		sql = "SELECT session_id,package,'',kind,title,"
		      "body,author,hostname,username,cwd,agent,ctx_origin,"
		      "created_at FROM notes WHERE id=?1";
	else if (strcmp(table, "policy_records") == 0)
		sql = "SELECT session_id,package,'','policy',check_name,"
		      "evidence_json,author,hostname,username,cwd,agent,"
		      "ctx_origin,created_at FROM policy_records WHERE id=?1";
	else if (strcmp(table, "tool_runs") == 0)
		sql = "SELECT session_id,'','','tool-run',tool,"
		      "argv,author,hostname,username,cwd,agent,ctx_origin,"
		      "created_at FROM tool_runs WHERE id=?1";
	else if (strcmp(table, "session_events") == 0)
		sql = "SELECT session_id,package,label,event,title,"
		      "body,author,hostname,username,cwd,agent,ctx_origin,"
		      "created_at FROM session_events WHERE id=?1";
	else
		return 0;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_int64(st, 1, id);
	if (sqlite3_step(st) != SQLITE_ROW) {
		sqlite3_finalize(st);
		return 0;
	}
	r->session = tr_dup(sqlite3_column_text(st, 0));
	r->package = tr_dup(sqlite3_column_text(st, 1));
	r->label = tr_dup(sqlite3_column_text(st, 2));
	r->kind = tr_dup(sqlite3_column_text(st, 3));
	r->title = tr_dup(sqlite3_column_text(st, 4));
	r->body = tr_dup(sqlite3_column_text(st, 5));
	r->author = tr_dup(sqlite3_column_text(st, 6));
	r->host = tr_dup(sqlite3_column_text(st, 7));
	r->user = tr_dup(sqlite3_column_text(st, 8));
	r->cwd = tr_dup(sqlite3_column_text(st, 9));
	r->agent = tr_dup(sqlite3_column_text(st, 10));
	r->origin = tr_dup(sqlite3_column_text(st, 11));
	r->created = tr_dup(sqlite3_column_text(st, 12));
	r->table = tr_dup((const unsigned char *)table);
	r->sid = id;
	sqlite3_finalize(st);
	if (!r->session || !r->package || !r->label || !r->kind ||
	    !r->title || !r->body || !r->author || !r->host ||
	    !r->user || !r->cwd || !r->agent || !r->origin ||
	    !r->created || !r->table) {
		tr_free_row(r);
		memset(r, 0, sizeof *r);
		return 0;
	}
	return 1;
}

/* Print one result row. Summary shows the title line plus snippet
 * (or body preview); full prints the whole body indented. */
static void tr_print_row(gbuf *out, const tr_row *r, int full, int env) {
	char tmp[128];
	snprintf(tmp, sizeof tmp, "- [%s #%lld]",
	         r->table, r->sid);
	gbuf_adds(out, tmp);
	if (r->label[0]) {
		gbuf_adds(out, " label '");
		gbuf_adds(out, r->label);
		gbuf_adds(out, "'");
	}
	if (r->title[0]) {
		gbuf_adds(out, " ");
		gbuf_adds(out, r->title);
	}
	gbuf_adds(out, " | session ");
	gbuf_adds(out, r->session[0] ? r->session : "-");
	gbuf_adds(out, " ");
	gbuf_adds(out, r->created);
	snprintf(tmp, sizeof tmp, " env:%d", env);
	gbuf_adds(out, tmp);
	if (r->agent[0]) {
		gbuf_adds(out, " agent ");
		gbuf_adds(out, r->agent);
	}
	gbuf_addch(out, '\n');
	if (full) {
		indented(out, r->body);
	} else if (r->snippet && r->snippet[0]) {
		gbuf_adds(out, "  ");
		gbuf_adds(out, r->snippet);
		gbuf_addch(out, '\n');
	} else {
		size_t n = strlen(r->body);
		gbuf_adds(out, "  ");
		if (n > 160) {
			gbuf_addn(out, r->body, 160);
			gbuf_adds(out, "...\n");
		} else {
			gbuf_adds(out, r->body);
			gbuf_addch(out, '\n');
		}
	}
}

/* Print one result row as a JSON line (export mode). */
static void tr_print_json(gbuf *out, const tr_row *r, int env) {
	char tmp[128];
	gbuf_adds(out, "{\"table\":\"");
	gbuf_adds(out, r->table);
	gbuf_adds(out, "\",\"id\":");
	snprintf(tmp, sizeof tmp, "%lld", r->sid);
	gbuf_adds(out, tmp);
	gbuf_adds(out, ",\"created_at\":\"");
	gbuf_adds(out, r->created);
	gbuf_adds(out, "\",\"session\":\"");
	gbuf_adds(out, r->session);
	gbuf_adds(out, "\",\"package\":\"");
	gbuf_adds(out, r->package);
	gbuf_adds(out, "\",\"label\":\"");
	gbuf_adds(out, r->label);
	gbuf_adds(out, "\",\"kind\":\"");
	gbuf_adds(out, r->kind);
	gbuf_adds(out, "\",\"title\":");
	json_escape(out, r->title, strlen(r->title));
	gbuf_adds(out, ",\"body\":");
	json_escape(out, r->body, strlen(r->body));
	gbuf_adds(out, ",\"author\":\"");
	gbuf_adds(out, r->author);
	gbuf_adds(out, "\",\"host\":\"");
	gbuf_adds(out, r->host);
	gbuf_adds(out, "\",\"user\":\"");
	gbuf_adds(out, r->user);
	gbuf_adds(out, "\",\"cwd\":\"");
	gbuf_adds(out, r->cwd);
	gbuf_adds(out, "\",\"agent\":\"");
	gbuf_adds(out, r->agent);
	gbuf_adds(out, "\",\"origin\":\"");
	gbuf_adds(out, r->origin);
	snprintf(tmp, sizeof tmp, "\",\"env\":%d}\n", env);
	gbuf_adds(out, tmp);
}

/* Environment index for compact responses: distinct
 * (host,user,cwd,origin) tuples printed once, rows reference by
 * number. Returns the index (appends when new). */
static int tr_env_index(char eh[][256], char eu[][256], char ed[][1024],
                        char eo[][16], int *nenv, const tr_row *r) {
	int i;
	for (i = 0; i < *nenv; i++) {
		if (strcmp(eh[i], r->host) == 0 &&
		    strcmp(eu[i], r->user) == 0 &&
		    strcmp(ed[i], r->cwd) == 0 &&
		    strcmp(eo[i], r->origin) == 0)
			return i + 1;
	}
	if (*nenv >= 100)
		return 1;
	snprintf(eh[*nenv], 256, "%s", r->host);
	snprintf(eu[*nenv], 256, "%s", r->user);
	snprintf(ed[*nenv], 1024, "%s", r->cwd);
	snprintf(eo[*nenv], 16, "%s", r->origin);
	(*nenv)++;
	return *nenv;
}

static void tr_print_envs(gbuf *out, char eh[][256], char eu[][256],
                          char ed[][1024], char eo[][16], int nenv) {
	int i;
	char tmp[64];
	snprintf(tmp, sizeof tmp, "environments (%d):\n", nenv);
	gbuf_adds(out, tmp);
	for (i = 0; i < nenv; i++) {
		snprintf(tmp, sizeof tmp, "  [%d] ", i + 1);
		gbuf_adds(out, tmp);
		if (eh[i][0] || eu[i][0] || ed[i][0]) {
			gbuf_adds(out, eh[i][0] ? eh[i] : "?");
			if (eu[i][0]) {
				gbuf_adds(out, " as ");
				gbuf_adds(out, eu[i]);
			}
			if (ed[i][0]) {
				gbuf_adds(out, " in ");
				gbuf_adds(out, ed[i]);
			}
			gbuf_adds(out, " (");
			gbuf_adds(out, eo[i][0] ? eo[i] : "?");
			gbuf_adds(out, ")\n");
		} else {
			gbuf_adds(out, "unknown origin\n");
		}
	}
}

static int t_total_recall(jv *args, gbuf *text, int *is_error, char *err, size_t errsz) {
	const char *mode, *query, *session, *pkg, *label, *proj, *aa;
	const char *detail, *budget, *cursor;
	const char *fh, *fu, *fc;
	long limit, tier;
	int bad = 0, full = 0, is_export = 0, is_query = 0;
	char dberr[256];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	tr_row *rows = NULL;
	int nrows = 0, scanned = 0;
	char eh[100][256], eu[100][256], ed[100][1024], eo[100][16];
	int nenv = 0;
	gbuf out;
	char tmp[256];
	char created[64] = "", ctable[32] = "";
	long long csid = 0;
	long coffset = 0;
	int have_cursor = 0;
	(void)err;
	(void)errsz;
	mode = arg_str(args, "mode");
	if (!mode || !mode[0])
		mode = "timeline";
	if (strcmp(mode, "query") != 0 && strcmp(mode, "timeline") != 0 &&
	    strcmp(mode, "export") != 0)
		return -1;
	is_query = strcmp(mode, "query") == 0;
	is_export = strcmp(mode, "export") == 0;
	query = arg_str(args, "query");
	if (is_query && (!query || !query[0]))
		return -1;
	session = arg_str(args, "session_id");
	pkg = arg_str(args, "package");
	label = arg_str(args, "label");
	proj = arg_str(args, "project");
	aa = arg_str(args, "author");
	fh = arg_str(args, "hostname");
	fu = arg_str(args, "username");
	fc = arg_str(args, "cwd");
	if ((session && strlen(session) > SESSION_CAP) ||
	    (pkg && strlen(pkg) > 512) || (label && strlen(label) > 512) ||
	    (proj && strlen(proj) > 512) || (aa && strlen(aa) > CREDIT_CAP) ||
	    (query && strlen(query) > 512) ||
	    (fh && strlen(fh) > PROV_HOST_CAP) ||
	    (fu && strlen(fu) > PROV_USER_CAP) ||
	    (fc && strlen(fc) > PROV_CWD_CAP))
		return -1;
	detail = arg_str(args, "detail");
	if (!detail || !detail[0])
		detail = "summary";
	if (strcmp(detail, "summary") != 0 && strcmp(detail, "full") != 0)
		return -1;
	full = strcmp(detail, "full") == 0;
	budget = arg_str(args, "budget");
	tier = tr_tier_bytes(budget);
	if (tier < 0)
		return -1;
	limit = clamp_long(arg_long(args, "limit", TR_LIMIT_DFLT, &bad), 1, TR_LIMIT_MAX);
	if (bad)
		return -1;
	cursor = arg_str(args, "cursor");
	if (cursor && cursor[0]) {
		int kind = tr_parse_cursor(cursor, created, sizeof created,
		                           ctable, sizeof ctable, &csid,
		                           &coffset);
		if (kind == 0)
			return -1;
		have_cursor = kind;
	}
	db = db_open_notes(dberr, sizeof dberr);
	if (!db) {
		*is_error = 1;
		gbuf_adds(text, "journal unavailable: ");
		gbuf_adds(text, dberr);
		gbuf_addch(text, '\n');
		prov(text, "notes.db (local journal)");
		return 0;
	}
	rows = calloc((size_t)limit, sizeof *rows);
	if (!rows) {
		sqlite3_close(db);
		*is_error = 1;
		gbuf_adds(text, "internal error: out of memory\n");
		return 0;
	}
	if (is_query) {
		char match[2048];
		if (tr_match_expr(query, match, sizeof match) != 0) {
			free(rows);
			sqlite3_close(db);
			return -1;
		}
		if (db_fts_ok()) {
			int rc = sqlite3_prepare_v2(db,
			    "SELECT source_table,source_id,"
			    "snippet(recall_index,5,'[',']','...',48)"
			    " FROM recall_index WHERE recall_index MATCH ?1"
			    " AND (?2 IS NULL OR session_id = ?2)"
			    " AND (?3 IS NULL OR package = ?3)"
			    " AND (?4 IS NULL OR label = ?4)"
			    " ORDER BY bm25(recall_index),source_table,source_id"
			    " LIMIT ?5 OFFSET ?6", -1, &st, NULL);
			if (rc != SQLITE_OK) {
				free(rows);
				sqlite3_close(db);
				*is_error = 1;
				gbuf_adds(text, "journal query failed\n");
				return 0;
			}
			sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
			if (session && session[0])
				sqlite3_bind_text(st, 2, session, -1, SQLITE_TRANSIENT);
			else
				sqlite3_bind_null(st, 2);
			if (pkg && pkg[0])
				sqlite3_bind_text(st, 3, pkg, -1, SQLITE_TRANSIENT);
			else
				sqlite3_bind_null(st, 3);
			if (label && label[0])
				sqlite3_bind_text(st, 4, label, -1, SQLITE_TRANSIENT);
			else
				sqlite3_bind_null(st, 4);
			sqlite3_bind_int(st, 5, (int)limit * 10);
			sqlite3_bind_int(st, 6, (int)(have_cursor == 1 ? coffset : 0));
			while (nrows < limit &&
			    sqlite3_step(st) == SQLITE_ROW) {
				const unsigned char *t =
				    sqlite3_column_text(st, 0);
				long long id = sqlite3_column_int64(st, 1);
				scanned++;
				if (!t)
					continue;
				if (!tr_fetch(db, (const char *)t, id,
				    &rows[nrows]))
					continue;
				rows[nrows].snippet =
				    tr_dup(sqlite3_column_text(st, 2));
				if (!rows[nrows].snippet) {
					tr_free_rows(rows, nrows + 1);
					sqlite3_finalize(st);
					sqlite3_close(db);
					*is_error = 1;
					gbuf_adds(text,
					    "internal error: out of memory\n");
					return 0;
				}
				/* Filters on fields outside the index
				 * apply at fetch time. */
				if (proj && proj[0] &&
				    strcmp(rows[nrows].table,
				    "checkpoints") != 0) {
					tr_free_row(&rows[nrows]);
					memset(&rows[nrows], 0,
					    sizeof rows[nrows]);
					continue;
				}
				if (aa && aa[0] &&
				    strcmp(rows[nrows].author, aa) != 0) {
					tr_free_row(&rows[nrows]);
					memset(&rows[nrows], 0,
					    sizeof rows[nrows]);
					continue;
				}
				if ((fh && fh[0] &&
				    strcmp(rows[nrows].host, fh) != 0) ||
				    (fu && fu[0] &&
				    strcmp(rows[nrows].user, fu) != 0) ||
				    (fc && fc[0] &&
				    strcmp(rows[nrows].cwd, fc) != 0)) {
					tr_free_row(&rows[nrows]);
					memset(&rows[nrows], 0,
					    sizeof rows[nrows]);
					continue;
				}
				nrows++;
			}
			scanned = (have_cursor == 1 ? (int)coffset : 0) + scanned;
		} else {
			char like[520], esc[512];
			tr_like_esc(query, esc, sizeof esc);
			snprintf(like, sizeof like, "%%%s%%", esc);
			/* No FTS5 on this host: substring fallback over
			 * base tables, newest first (unranked). */
			int rc = sqlite3_prepare_v2(db,
			    "SELECT 'checkpoints',id,NULL FROM checkpoints"
			    " WHERE title LIKE ?1 ESCAPE '\\'"
			    " OR body LIKE ?1 ESCAPE '\\'"
			    " UNION ALL SELECT 'notes',id,NULL FROM notes"
			    " WHERE title LIKE ?1 ESCAPE '\\'"
			    " OR body LIKE ?1 ESCAPE '\\'"
			    " UNION ALL SELECT 'policy_records',id,NULL"
			    " FROM policy_records"
			    " WHERE check_name LIKE ?1 ESCAPE '\\'"
			    " OR evidence_json LIKE ?1 ESCAPE '\\'"
			    " UNION ALL SELECT 'tool_runs',id,NULL"
			    " FROM tool_runs WHERE tool LIKE ?1 ESCAPE '\\'"
			    " OR argv LIKE ?1 ESCAPE '\\'"
			    " UNION ALL SELECT 'session_events',id,NULL"
			    " FROM session_events WHERE title LIKE ?1 ESCAPE '\\'"
			    " OR body LIKE ?1 ESCAPE '\\'"
			    " ORDER BY 1,2 LIMIT ?2",
			    -1, &st, NULL);
			if (rc != SQLITE_OK) {
				free(rows);
				sqlite3_close(db);
				*is_error = 1;
				gbuf_adds(text, "journal query failed\n");
				return 0;
			}
			sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(st, 2, (int)limit * 10);
			while (nrows < limit &&
			    sqlite3_step(st) == SQLITE_ROW) {
				const unsigned char *t =
				    sqlite3_column_text(st, 0);
				long long id = sqlite3_column_int64(st, 1);
				scanned++;
				if (!t)
					continue;
				if (!tr_fetch(db, (const char *)t, id,
				    &rows[nrows]))
					continue;
				nrows++;
			}
		}
		sqlite3_finalize(st);
		st = NULL;
	} else {
		/* Timeline and export share one chronological UNION.
		 * A filter on a field a table lacks excludes that
		 * table's rows; tool runs carry no package/label. */
		int rc = sqlite3_prepare_v2(db,
		    "SELECT created_at,tname,sid,session_id,package,label,"
		    "kind,title,body,author,host,user,cwd,agent,origin"
		    " FROM ("
		    "SELECT created_at,'checkpoints' AS tname,id AS sid,"
		    "session_id,package,label,'checkpoint' AS kind,title,"
		    "body,author,hostname AS host,username AS user,cwd,"
		    "agent,ctx_origin AS origin FROM checkpoints"
		    " WHERE (?1 IS NULL OR session_id = ?1)"
		    " AND (?2 IS NULL OR package = ?2)"
		    " AND (?3 IS NULL OR label = ?3)"
		    " AND (?4 IS NULL OR project = ?4)"
		    " AND (?5 IS NULL OR author = ?5)"
		    " AND (?6 IS NULL OR hostname = ?6)"
		    " AND (?7 IS NULL OR username = ?7)"
		    " AND (?8 IS NULL OR cwd = ?8)"
		    " UNION ALL SELECT created_at,'notes',id,"
		    "session_id,package,'',kind,title,"
		    "body,author,hostname,username,cwd,"
		    "agent,ctx_origin FROM notes"
		    " WHERE (?1 IS NULL OR session_id = ?1)"
		    " AND (?2 IS NULL OR package = ?2)"
		    " AND ?3 IS NULL"
		    " AND ?4 IS NULL"
		    " AND (?5 IS NULL OR author = ?5)"
		    " AND (?6 IS NULL OR hostname = ?6)"
		    " AND (?7 IS NULL OR username = ?7)"
		    " AND (?8 IS NULL OR cwd = ?8)"
		    " UNION ALL SELECT created_at,'policy_records',id,"
		    "session_id,package,'','policy',check_name,"
		    "evidence_json,author,hostname,username,cwd,"
		    "agent,ctx_origin FROM policy_records"
		    " WHERE (?1 IS NULL OR session_id = ?1)"
		    " AND (?2 IS NULL OR package = ?2)"
		    " AND ?3 IS NULL"
		    " AND ?4 IS NULL"
		    " AND (?5 IS NULL OR author = ?5)"
		    " AND (?6 IS NULL OR hostname = ?6)"
		    " AND (?7 IS NULL OR username = ?7)"
		    " AND (?8 IS NULL OR cwd = ?8)"
		    " UNION ALL SELECT created_at,'tool_runs',id,"
		    "session_id,'','','tool-run',tool,"
		    "argv,author,hostname,username,cwd,"
		    "agent,ctx_origin FROM tool_runs"
		    " WHERE (?1 IS NULL OR session_id = ?1)"
		    " AND ?2 IS NULL"
		    " AND ?3 IS NULL"
		    " AND ?4 IS NULL"
		    " AND (?5 IS NULL OR author = ?5)"
		    " AND (?6 IS NULL OR hostname = ?6)"
		    " AND (?7 IS NULL OR username = ?7)"
		    " AND (?8 IS NULL OR cwd = ?8)"
		    " UNION ALL SELECT created_at,'session_events',id,"
		    "session_id,package,label,event,title,"
		    "body,author,hostname,username,cwd,"
		    "agent,ctx_origin FROM session_events"
		    " WHERE (?1 IS NULL OR session_id = ?1)"
		    " AND (?2 IS NULL OR package = ?2)"
		    " AND (?3 IS NULL OR label = ?3)"
		    " AND ?4 IS NULL"
		    " AND (?5 IS NULL OR author = ?5)"
		    " AND (?6 IS NULL OR hostname = ?6)"
		    " AND (?7 IS NULL OR username = ?7)"
		    " AND (?8 IS NULL OR cwd = ?8)"
		    ") WHERE (?9 IS NULL OR (created_at > ?9 OR ("
		    "created_at = ?9 AND (tname > ?10 OR (tname = ?10 AND "
		    "sid > ?11)))))"
		    " ORDER BY created_at,tname,sid LIMIT ?12",
		    -1, &st, NULL);
		const char *f[8];
		int i;
		if (rc != SQLITE_OK) {
			free(rows);
			sqlite3_close(db);
			*is_error = 1;
			gbuf_adds(text, "journal query failed\n");
			return 0;
		}
		f[0] = (session && session[0]) ? session : NULL;
		f[1] = (pkg && pkg[0]) ? pkg : NULL;
		f[2] = (label && label[0]) ? label : NULL;
		f[3] = (proj && proj[0]) ? proj : NULL;
		f[4] = (aa && aa[0]) ? aa : NULL;
		f[5] = (fh && fh[0]) ? fh : NULL;
		f[6] = (fu && fu[0]) ? fu : NULL;
		f[7] = (fc && fc[0]) ? fc : NULL;
		for (i = 0; i < 8; i++) {
			if (f[i])
				sqlite3_bind_text(st, i + 1, f[i], -1,
				                  SQLITE_TRANSIENT);
			else
				sqlite3_bind_null(st, i + 1);
		}
		if (have_cursor == 2) {
			sqlite3_bind_text(st, 9, created, -1,
			                  SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 10, ctable, -1,
			                  SQLITE_TRANSIENT);
			sqlite3_bind_int64(st, 11, csid);
		} else {
			sqlite3_bind_null(st, 9);
			sqlite3_bind_null(st, 10);
			sqlite3_bind_null(st, 11);
		}
		sqlite3_bind_int(st, 12, (int)limit);
		while (nrows < limit && sqlite3_step(st) == SQLITE_ROW) {
			tr_row *r = &rows[nrows];
			r->created = tr_dup(sqlite3_column_text(st, 0));
			r->table = tr_dup(sqlite3_column_text(st, 1));
			r->sid = sqlite3_column_int64(st, 2);
			r->session = tr_dup(sqlite3_column_text(st, 3));
			r->package = tr_dup(sqlite3_column_text(st, 4));
			r->label = tr_dup(sqlite3_column_text(st, 5));
			r->kind = tr_dup(sqlite3_column_text(st, 6));
			r->title = tr_dup(sqlite3_column_text(st, 7));
			r->body = tr_dup(sqlite3_column_text(st, 8));
			r->author = tr_dup(sqlite3_column_text(st, 9));
			r->host = tr_dup(sqlite3_column_text(st, 10));
			r->user = tr_dup(sqlite3_column_text(st, 11));
			r->cwd = tr_dup(sqlite3_column_text(st, 12));
			r->agent = tr_dup(sqlite3_column_text(st, 13));
			r->origin = tr_dup(sqlite3_column_text(st, 14));
			r->rank = 0;
			r->snippet = tr_dup((const unsigned char *)"");
			if (!r->created || !r->table || !r->session ||
			    !r->package || !r->label || !r->kind ||
			    !r->title || !r->body || !r->author ||
			    !r->host || !r->user || !r->cwd || !r->agent ||
			    !r->origin || !r->snippet) {
				tr_free_rows(rows, nrows + 1);
				sqlite3_finalize(st);
				sqlite3_close(db);
				*is_error = 1;
				gbuf_adds(text,
				    "internal error: out of memory\n");
				return 0;
			}
			nrows++;
		}
		sqlite3_finalize(st);
		st = NULL;
	}
	sqlite3_close(db);
	/* Assemble within budget: header, one environments block,
	 * then rows until the tier would overflow. */
	gbuf_init(&out);
	{
		const char *tiername = !budget || !budget[0] ? "compact" :
		    budget;
		long kib = tier / 1024;
		snprintf(tmp, sizeof tmp,
		    "total_recall %s, budget %s (%ldKiB), %d row%s:\n",
		    mode, tiername, kib, nrows, nrows == 1 ? "" : "s");
		gbuf_adds(&out, tmp);
		gbuf_adds(&out, "  scope:");
		if (session && session[0]) {
			gbuf_adds(&out, " session ");
			gbuf_adds(&out, session);
		}
		if (pkg && pkg[0]) {
			gbuf_adds(&out, " package ");
			gbuf_adds(&out, pkg);
		}
		if (label && label[0]) {
			gbuf_adds(&out, " label ");
			gbuf_adds(&out, label);
		}
		if (proj && proj[0]) {
			gbuf_adds(&out, " project ");
			gbuf_adds(&out, proj);
		}
		if (aa && aa[0]) {
			gbuf_adds(&out, " author ");
			gbuf_adds(&out, aa);
		}
		if (is_query) {
			gbuf_adds(&out, " query ");
			gbuf_adds(&out, query);
		}
		gbuf_addch(&out, '\n');
		gbuf_adds(&out, "  searched: checkpoints, notes, policy"
		    " records, tool runs, session events\n");
		gbuf_adds(&out, "  not searched: tool outputs and"
		    " conversation turns (never captured)\n");
	}
	if (nrows == 0) {
		gbuf_adds(&out, "no matching records\ncursor: end\n");
	} else {
		int i, emitted = 0;
		int env_of[100];
		for (i = 0; i < nrows; i++)
			env_of[i] = tr_env_index(eh, eu, ed, eo, &nenv,
			                         &rows[i]);
		tr_print_envs(&out, eh, eu, ed, eo, nenv);
		if (is_export) {
			gbuf_adds(&out, "{\"environments\":[");
			for (i = 0; i < nenv; i++) {
				char ebuf[128];
				if (i)
					gbuf_addch(&out, ',');
				snprintf(ebuf, sizeof ebuf,
				    "{\"n\":%d}", i + 1);
				gbuf_adds(&out, ebuf);
			}
			gbuf_adds(&out, "]}\n");
		}
		for (i = 0; i < nrows; i++) {
			gbuf row;
			gbuf_init(&row);
			if (is_export)
				tr_print_json(&row, &rows[i], env_of[i]);
			else
				tr_print_row(&row, &rows[i], full,
				             env_of[i]);
			/* The first row is always emitted: a record must
			 * never be invisible, and its cursor lets the
			 * caller step past it with a bigger budget. */
			if ((!row.failed && row.p &&
			    out.len + row.len <= (size_t)tier) ||
			    emitted == 0) {
				gbuf_addn(&out, row.p, row.len);
				emitted++;
			} else {
				gbuf_free(&row);
				break;
			}
			gbuf_free(&row);
		}
		if (emitted < nrows || nrows == (int)limit) {
			char cur[160];
			if (is_query) {
				long adv = (have_cursor == 1 ? coffset : 0L)
				    + scanned;
				snprintf(cur, sizeof cur, "o%ld", adv);
			} else {
				tr_row *last = &rows[emitted > 0 ?
				    emitted - 1 : 0];
				snprintf(cur, sizeof cur, "%s|%s|%lld",
				         last->created, last->table,
				         last->sid);
			}
			/* Limit reached with budget to spare is a page
			 * boundary, not a truncation; cursors assume a
			 * quiet journal (append-only, but new rows can
			 * shift later pages). */
			snprintf(tmp, sizeof tmp, "%s cursor=%s]\n",
			    emitted < nrows ?
			    "[truncated: budget exhausted," :
			    "[page complete:",
			    cur);
			if (out.len + strlen(tmp) <= (size_t)tier + 256)
				gbuf_adds(&out, tmp);
			else
				gbuf_adds(&out, "[truncated]\n");
		} else {
			gbuf_adds(&out, "cursor: end\n");
		}
		if (out.failed) {
			tr_free_rows(rows, nrows);
			gbuf_free(&out);
			*is_error = 1;
			gbuf_adds(text, "internal error: out of memory\n");
			return 0;
		}
	}
	tr_free_rows(rows, nrows);
	gbuf_adds(text, out.p ? out.p : "");
	gbuf_free(&out);
	prov(text, "notes.db (local journal; recorder is client-reported, unverified)");
	if (text->failed)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* tools.conf extensions                                                */
/* ------------------------------------------------------------------ */
/* The 23 built-in entries, in historical order. tools_list_json emits
 * these (minus expose-offs) followed by registered externals, so the
 * no-conf document is byte-identical to the pre-0.8.0 static text. */
static const struct {
	const char *name;
	const char *entry;
} base_tools[] = {
	{ "package_search", "{\"name\":\"package_search\",\"description\":\"Find packages by name or description substring.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"},\"include_disabled\":{\"type\":\"boolean\"}},\"required\":[\"query\"]}}" },
	{ "package_info", "{\"name\":\"package_info\",\"description\":\"Recipe fields and repository newest for a package.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}" },
	{ "package_dependencies", "{\"name\":\"package_dependencies\",\"description\":\"Dependency edges for a package.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"direction\":{\"type\":\"string\",\"enum\":[\"deps\",\"reverse\",\"both\"]}},\"required\":[\"name\"]}}" },
	{ "package_why_installed", "{\"name\":\"package_why_installed\",\"description\":\"Explain dependency chain(s) from a package to a dependency.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"dep\":{\"type\":\"string\"}},\"required\":[\"package\",\"dep\"]}}" },
	{ "package_installed", "{\"name\":\"package_installed\",\"description\":\"Installed version versus repository newest.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}" },
	{ "package_build_status", "{\"name\":\"package_build_status\",\"description\":\"Latest build verdict, timings and detail.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"include_disabled\":{\"type\":\"boolean\"}},\"required\":[\"name\"]}}" },
	{ "package_build_history", "{\"name\":\"package_build_history\",\"description\":\"Previous build attempts with timings and SBU.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"},\"include_disabled\":{\"type\":\"boolean\"}},\"required\":[\"name\"]}}" },
	{ "package_build_log", "{\"name\":\"package_build_log\",\"description\":\"Retained build/install log tail.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"stage\":{\"type\":\"string\",\"enum\":[\"build\",\"install\",\"both\"]},\"tail_bytes\":{\"type\":\"number\"}},\"required\":[\"name\"]}}" },
	{ "package_artifacts", "{\"name\":\"package_artifacts\",\"description\":\"Staged, signed and published artifacts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}" },
	{ "build_queue", "{\"name\":\"build_queue\",\"description\":\"Queued and active builds.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"number\"}}}}" },
	{ "package_identity", "{\"name\":\"package_identity\",\"description\":\"Unix identity: fragment, registry, reservations, live accounts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}" },
	{ "package_notes_get", "{\"name\":\"package_notes_get\",\"description\":\"Read engineering notes for a package (newest first).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"finding\",\"decision\",\"issue\",\"success\",\"failure\",\"lesson\"]},\"limit\":{\"type\":\"number\"}},\"required\":[\"package\"]}}" },
	{ "package_notes_add", "{\"name\":\"package_notes_add\",\"description\":\"Record an engineering finding, decision, open issue, success, failure or lesson (journal only).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"finding\",\"decision\",\"issue\",\"success\",\"failure\",\"lesson\"]},\"title\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"},\"session_id\":{\"type\":\"string\"},\"hostname\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"agent\":{\"type\":\"string\"}},\"required\":[\"package\",\"kind\",\"body\"]}}" },
	{ "package_policy_status", "{\"name\":\"package_policy_status\",\"description\":\"Policy checks with evidence and freshness for a package.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"}},\"required\":[\"package\"]}}" },
	{ "package_policy_record", "{\"name\":\"package_policy_record\",\"description\":\"Attach verified evidence to a policy check (journal only).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"check\":{\"type\":\"string\",\"enum\":[\"unix-identity\",\"payload-layout\",\"revision-discipline\",\"dependency-closure\",\"signing-acceptance\"]},\"result\":{\"type\":\"string\",\"enum\":[\"PASS\",\"FAIL\"]},\"evidence\":{\"type\":\"object\"},\"author\":{\"type\":\"string\"},\"session_id\":{\"type\":\"string\"},\"hostname\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"agent\":{\"type\":\"string\"}},\"required\":[\"package\",\"check\",\"result\"]}}" },
	{ "package_readiness", "{\"name\":\"package_readiness\",\"description\":\"What is verified, stale, unverified, plus open issues and recent notes.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"}},\"required\":[\"package\"]}}" },
	{ "build_request", "{\"name\":\"build_request\",\"description\":\"Queue a build through the daemon intake (scan); the daemon executes detached. Records the reason in the journal.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"}},\"required\":[\"package\",\"reason\"]}}" },
	{ "package_bump", "{\"name\":\"package_bump\",\"description\":\"Revision bump with bumppkg guards; the reason is journaled. Never signs, publishes or installs.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"}},\"required\":[\"package\",\"reason\"]}}" },
	{ "checkpoint_save", "{\"name\":\"checkpoint_save\",\"description\":\"Save a pre-restart session checkpoint (journal only, active by default). Resume post-restart with checkpoint_resume.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"package\":{\"type\":\"string\"},\"project\":{\"type\":\"string\"},\"workstream\":{\"type\":\"string\"},\"src_commit\":{\"type\":\"string\"},\"recipe_commit\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"},\"session_id\":{\"type\":\"string\"},\"hostname\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"agent\":{\"type\":\"string\"}},\"required\":[\"label\",\"body\"]}}" },
	{ "checkpoint_resume", "{\"name\":\"checkpoint_resume\",\"description\":\"Resume a saved checkpoint by id, by label (newest), or newest overall when neither is given. Closed checkpoints stay resumable by id; checkpoint_list is the active register.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"number\"},\"label\":{\"type\":\"string\"}}}}" },
	{ "checkpoint_list", "{\"name\":\"checkpoint_list\",\"description\":\"List the active checkpoint register (newest first, with total count). Closed work is hidden unless status is closed/all or include_closed is set.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"enum\":[\"active\",\"closed\",\"all\"]},\"project\":{\"type\":\"string\"},\"workstream\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"},\"include_closed\":{\"type\":\"boolean\"}}}}" },
	{ "checkpoint_close", "{\"name\":\"checkpoint_close\",\"description\":\"Retire a finished checkpoint from the active register by id. Never deletes: the row stays resumable by id.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"number\"}},\"required\":[\"id\"]}}" },
	{ "total_recall", "{\"name\":\"total_recall\",\"description\":\"Bounded historical retrieval over the journal: ranked query, chronological timeline, or paginated export. Checkpoints restore working state cheaply; use this only when deeper evidence is required, never automatically.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"query\",\"timeline\",\"export\"]},\"query\":{\"type\":\"string\"},\"session_id\":{\"type\":\"string\"},\"package\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"},\"project\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"},\"hostname\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"detail\":{\"type\":\"string\",\"enum\":[\"summary\",\"full\"]},\"budget\":{\"type\":\"string\",\"enum\":[\"compact\",\"standard\",\"extended\",\"deep\"]},\"limit\":{\"type\":\"number\"},\"cursor\":{\"type\":\"string\"}}}}" },
};
#define NBASE (int)(sizeof base_tools / sizeof base_tools[0])

static int is_base(const char *name) {
	int i;
	for (i = 0; i < NBASE; i++) {
		if (strcmp(base_tools[i].name, name) == 0)
			return 1;
	}
	return 0;
}

/* Cross-checks conf.c cannot do: expose targets must be real
 * built-ins and never the checkpoint register; external names must
 * not shadow built-ins. Violations invalidate extensions (built-ins
 * stay fully on) rather than failing the server. */
static void tools_validate(ext_conf *c) {
	int i;
	if (!c->ok || !c->present)
		return;
	for (i = 0; i < c->noff; i++) {
		if (!is_base(c->off[i]) ||
		    strncmp(c->off[i], "checkpoint_", 11) == 0) {
			snprintf(c->err, sizeof c->err,
			         "expose refused for '%s'", c->off[i]);
			c->ok = 0;
			return;
		}
	}
	for (i = 0; i < c->ntools; i++) {
		if (is_base(c->tools[i].name)) {
			snprintf(c->err, sizeof c->err,
			         "external shadows builtin '%s'",
			         c->tools[i].name);
			c->ok = 0;
			return;
		}
	}
}

static int ext_is_off(const ext_conf *c, const char *name) {
	int i;
	if (!c->ok)
		return 0;
	for (i = 0; i < c->noff; i++) {
		if (strcmp(c->off[i], name) == 0)
			return 1;
	}
	return 0;
}

/* Portable mode (SBM_SAPHIRA=off): built-ins that require Saphira
 * builder state (/recipes tree, queue/repository/apk databases,
 * accounts registry, retained logs, incoming). Everything else —
 * checkpoints, the notes journal, registered externals — needs only
 * the journal database and stays on anywhere. */
static const char *saphira_gated[] = {
	"package_search", "package_info", "package_dependencies",
	"package_why_installed", "package_installed",
	"package_build_status", "package_build_history",
	"package_build_log", "package_artifacts",
	"build_queue", "package_identity",
	NULL
};

static int saphira_is_gated(const char *name) {
	int i;
	for (i = 0; saphira_gated[i]; i++) {
		if (strcmp(saphira_gated[i], name) == 0)
			return 1;
	}
	return 0;
}

/* Privileged build operations: queueing builds and bumping
 * revisions change the world beyond the journal, so they stand
 * behind their own capability. Everything else — checkpoints,
 * notes, policy records and status, readiness, total_recall —
 * stays on wherever the journal lives, independent of build
 * permission. Signing, publication and installation have no
 * tools at all and remain human gates by construction. */
static const char *build_gated[] = {
	"build_request", "package_bump",
	NULL
};

static int build_is_gated(const char *name) {
	int i;
	for (i = 0; build_gated[i]; i++) {
		if (strcmp(build_gated[i], name) == 0)
			return 1;
	}
	return 0;
}

void tools_list_json(gbuf *out) {
	ext_conf conf;
	int i, first = 1;
	conf_load(&conf);
	tools_validate(&conf);
	gbuf_adds(out, "{\"tools\":[");
	for (i = 0; i < NBASE; i++) {
		if (ext_is_off(&conf, base_tools[i].name))
			continue;
		if (saphira_mode() <= 0 && saphira_is_gated(base_tools[i].name))
			continue;
		if (build_mode() <= 0 && build_is_gated(base_tools[i].name))
			continue;
		if (!first)
			gbuf_addch(out, ',');
		gbuf_adds(out, base_tools[i].entry);
		first = 0;
	}
	if (conf.ok) {
		for (i = 0; i < conf.ntools; i++) {
			const ext_tool *t = &conf.tools[i];
			gbuf d;
			int a;
			if (!first)
				gbuf_addch(out, ',');
			gbuf_init(&d);
			gbuf_adds(&d, "External (");
			gbuf_adds(&d, t->cls);
			gbuf_adds(&d, "): ");
			for (a = 0; a < t->argc; a++) {
				if (a)
					gbuf_addch(&d, ' ');
				gbuf_adds(&d, t->argv[a]);
			}
			gbuf_adds(&d, " (tools.conf)");
			gbuf_adds(out, "{\"name\":");
			json_escape(out, t->name, strlen(t->name));
			gbuf_adds(out, ",\"description\":");
			json_escape(out, d.p ? d.p : "", d.len);
			gbuf_adds(out, ",\"inputSchema\":"
			               "{\"type\":\"object\",\"properties\":{}}}");
			gbuf_free(&d);
			first = 0;
		}
	}
	gbuf_adds(out, "]}");
	conf_free(&conf);
}

/* Registered externals take no caller arguments: the conf argv is
 * the whole command. An author field is still honored for audit
 * attribution when supplied. */
static int t_external(jv *args, const ext_tool *t, gbuf *text, int *is_error) {
	char author[128];
	resolve_credit(args, author, sizeof author);
	return runner_exec(t, author, journal_recorder, text, is_error);
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */
int tools_call(const char *name, jv *args, gbuf *text, int *is_error) {
	char err[256];
	ext_conf conf;
	*is_error = 0;
	if (!name)
		return -1;
	/* Exposure allowlist gates even built-ins: an off tool is
	 * unknown, not failed. Portable mode gates Saphira-state
	 * tools the same way (<= 0 fails closed toward gated;
	 * main() already refused an invalid value outright). */
	conf_load(&conf);
	tools_validate(&conf);
	if (ext_is_off(&conf, name)) {
		conf_free(&conf);
		return -1;
	}
	if (saphira_mode() <= 0 && saphira_is_gated(name)) {
		conf_free(&conf);
		return -1;
	}
	if (build_mode() <= 0 && build_is_gated(name)) {
		conf_free(&conf);
		return -1;
	}
	conf_free(&conf);
	if (strcmp(name, "package_search") == 0)
		return t_search(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_info") == 0)
		return t_info(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_dependencies") == 0)
		return t_deps(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_why_installed") == 0)
		return t_why(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_installed") == 0)
		return t_installed(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_build_status") == 0)
		return t_status(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_build_history") == 0)
		return t_history(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_build_log") == 0)
		return t_log(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_artifacts") == 0)
		return t_artifacts(args, text, is_error, err, sizeof err);
	if (strcmp(name, "build_queue") == 0)
		return t_queue(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_identity") == 0)
		return t_identity(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_notes_get") == 0)
		return t_notes_get(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_notes_add") == 0)
		return t_notes_add(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_policy_status") == 0)
		return t_policy_status(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_policy_record") == 0)
		return t_policy_record(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_readiness") == 0)
		return t_readiness(args, text, is_error, err, sizeof err);
	if (strcmp(name, "build_request") == 0)
		return t_build_request(args, text, is_error, err, sizeof err);
	if (strcmp(name, "package_bump") == 0)
		return t_package_bump(args, text, is_error, err, sizeof err);
	if (strcmp(name, "checkpoint_save") == 0)
		return t_checkpoint_save(args, text, is_error, err, sizeof err);
	if (strcmp(name, "checkpoint_resume") == 0)
		return t_checkpoint_resume(args, text, is_error, err, sizeof err);
	if (strcmp(name, "checkpoint_list") == 0)
		return t_checkpoint_list(args, text, is_error, err, sizeof err);
	if (strcmp(name, "checkpoint_close") == 0)
		return t_checkpoint_close(args, text, is_error, err, sizeof err);
	if (strcmp(name, "total_recall") == 0)
		return t_total_recall(args, text, is_error, err, sizeof err);
	/* Registered externals: fixed conf argv, no caller arguments. */
	{
		ext_conf conf2;
		const ext_tool *e;
		int r;
		conf_load(&conf2);
		tools_validate(&conf2);
		e = conf_find(&conf2, name);
		if (!e) {
			conf_free(&conf2);
			return -1;
		}
		r = t_external(args, e, text, is_error);
		conf_free(&conf2);
		return r;
	}
}



