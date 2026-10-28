#include "tools.h"
#include "db.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
}

/* ------------------------------------------------------------------ */
/* recipe scan: parsed fields, never sourced                           */
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
static void format_build(sqlite3 *db, sqlite3_stmt *st, gbuf *text) {
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
		format_build(db, st, text);
	} else {
		gbuf_adds(text, "never built: ");
		gbuf_adds(text, name);
		gbuf_addch(text, '\n');
	}
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
		format_build(db, st, text);
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
 * field of each fragment line (`user <name> ...`). The package name
 * itself is NOT an identity; looking live accounts up by package
 * name is how pkg-a once reported "absent" for a healthy pkguser. */
static int frag_idents(const char *fragp, size_t fraglen,
                       char out[][IDENT_LEN], int max) {
	int n = 0;
	const char *p = fragp, *end = fragp + fraglen;
	while (p < end && n < max) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *eol = nl ? nl : end;
		const char *s = p;
		while (s < eol && (*s == ' ' || *s == '\t' || *s == '\r'))
			s++;
		if (s < eol && *s != '#') {
			const char *ns;
			while (s < eol && *s != ' ' && *s != '\t' && *s != '\r')
				s++;
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
		}
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
				char *t1, *t2, *nm;
				size_t L = strlen(line);
				while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
					line[--L] = 0;
				if (!L || line[0] == '#')
					continue;
				/* registry rows are `kind\tname\t...`: the
				 * identity is the SECOND field, not the kind. */
				t1 = strchr(line, '\t');
				if (!t1)
					continue;
				nm = t1 + 1;
				t2 = strchr(nm, '\t');
				if (t2)
					*t2 = 0;
				if (strcmp(nm, name) == 0 ||
				    ident_match(idents, nidents, nm, strlen(nm))) {
					if (t2)
						*t2 = '\t';
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

static int kind_valid(const char *k) {
	return strcmp(k, "finding") == 0 || strcmp(k, "decision") == 0 ||
	       strcmp(k, "issue") == 0;
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

static void utc_now(char out[21]) {
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
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

/* sha256 hex of one file via the installed coreutils frontend. */
static int sha256_hex_file(const char *path, char out[65]) {
	char *av[4];
	gbuf so, se;
	int trunc = 0, rc, i;
	char ebuf[128];
	gbuf_init(&so);
	gbuf_init(&se);
	av[0] = "/bin/sha256sum";
	av[1] = "--";
	av[2] = (char *)path;
	av[3] = NULL;
	rc = run_capture(av, &so, &se, &trunc, ebuf, sizeof ebuf);
	if (rc == 0 && !so.failed && so.len >= 65) {
		for (i = 0; i < 64; i++) {
			char c = so.p[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
			      (c >= 'A' && c <= 'F')))
				break;
		}
		if (i == 64 && (so.p[64] == ' ' || so.p[64] == '\t')) {
			memcpy(out, so.p, 64);
			out[64] = 0;
			gbuf_free(&so);
			gbuf_free(&se);
			return 0;
		}
	}
	gbuf_free(&so);
	gbuf_free(&se);
	return -1;
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
	int rc;
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
	    "INSERT INTO notes(package,kind,title,body,author,recorder,recipe_commit,created_at)"
	    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8)", -1, &st, NULL);
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
	int rc;
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
	    "recorder,created_at,supersedes_id)"
	    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)", -1, &st, NULL);
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

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */
int tools_call(const char *name, jv *args, gbuf *text, int *is_error) {
	char err[256];
	*is_error = 0;
	if (!name)
		return -1;
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
	return -1;
}



