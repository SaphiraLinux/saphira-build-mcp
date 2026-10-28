#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static int jv_grow_items(jv *v) {
	if (v->ni == v->cap) {
		int nc = v->cap ? v->cap * 2 : 8;
		jv **p = realloc(v->items, (size_t)nc * sizeof(jv *));
		if (!p) return 0;
		v->cap = nc;
		v->items = p;
	}
	return 1;
}
static int jv_grow_kv(jv *v) {
	if (v->nk == v->kcap) {
		int nc = v->kcap ? v->kcap * 2 : 8;
		void *p = realloc(v->kv, (size_t)nc * sizeof(*v->kv));
		if (!p) return 0;
		v->kcap = nc;
		v->kv = p;
	}
	return 1;
}
static jv *jv_new(int t) {
	jv *v = calloc(1, sizeof *v);
	v->t = t;
	return v;
}
static void skipws(const char *s, size_t len, size_t *pos) {
	while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == '\n' || s[*pos] == '\r'))
		(*pos)++;
}
static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static size_t utf8_sequence(const unsigned char *p, size_t n) {
	if (n == 0 || p[0] < 0x80) return n ? 1 : 0;
	if (p[0] >= 0xC2 && p[0] <= 0xDF)
		return n >= 2 && (p[1] & 0xC0) == 0x80 ? 2 : 0;
	if (p[0] == 0xE0)
		return n >= 3 && p[1] >= 0xA0 && p[1] <= 0xBF && (p[2] & 0xC0) == 0x80 ? 3 : 0;
	if (p[0] >= 0xE1 && p[0] <= 0xEC)
		return n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 ? 3 : 0;
	if (p[0] == 0xED)
		return n >= 3 && p[1] >= 0x80 && p[1] <= 0x9F && (p[2] & 0xC0) == 0x80 ? 3 : 0;
	if (p[0] >= 0xEE && p[0] <= 0xEF)
		return n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 ? 3 : 0;
	if (p[0] == 0xF0)
		return n >= 4 && p[1] >= 0x90 && p[1] <= 0xBF &&
		       (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80 ? 4 : 0;
	if (p[0] >= 0xF1 && p[0] <= 0xF3)
		return n >= 4 && (p[1] & 0xC0) == 0x80 &&
		       (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80 ? 4 : 0;
	if (p[0] == 0xF4)
		return n >= 4 && p[1] >= 0x80 && p[1] <= 0x8F &&
		       (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80 ? 4 : 0;
	return 0;
}

static int utf8_valid(const char *s, size_t len) {
	for (size_t i = 0; i < len;) {
		size_t n = utf8_sequence((const unsigned char *)s + i, len - i);
		if (!n) return 0;
		i += n;
	}
	return 1;
}

static jv *parse_value(const char *s, size_t len, size_t *pos, int *err);

static jv *parse_string(const char *s, size_t len, size_t *pos, int *err) {
	(*pos)++;
	gbuf b;
	gbuf_init(&b);
	while (*pos < len) {
		char c = s[*pos];
		if (c == '"') {
			(*pos)++;
			if (b.failed) { gbuf_free(&b); *err = 1; return NULL; }
			jv *v = jv_new(3);
			if (!v) { gbuf_free(&b); *err = 1; return NULL; }
			v->str = b.p ? b.p : strdup("");
			if (!v->str) { free(v); *err = 1; return NULL; }
			return v;
		}
		if (c == '\\') {
			(*pos)++;
			if (*pos >= len) { *err = 1; break; }
			char e = s[*pos];
			(*pos)++;
			switch (e) {
			case '"':  gbuf_addch(&b, '"'); break;
			case '\\': gbuf_addch(&b, '\\'); break;
			case '/':  gbuf_addch(&b, '/'); break;
			case 'b':  gbuf_addch(&b, '\b'); break;
			case 'f':  gbuf_addch(&b, '\f'); break;
			case 'n':  gbuf_addch(&b, '\n'); break;
			case 'r':  gbuf_addch(&b, '\r'); break;
			case 't':  gbuf_addch(&b, '\t'); break;
			case 'u': {
				if (len - *pos < 4) { *err = 1; break; }
				int cp = 0, ok = 1;
				for (int i = 0; i < 4; i++) {
					int h = hexval(s[*pos + (size_t)i]);
					if (h < 0) { ok = 0; break; }
					cp = cp * 16 + h;
				}
				if (!ok) { *err = 1; break; }
				*pos += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					if (len - *pos < 6 || s[*pos] != '\\' || s[*pos + 1] != 'u') { *err = 1; break; }
					int low = 0;
					for (int i = 0; i < 4; i++) {
						int h = hexval(s[*pos + 2 + (size_t)i]);
						if (h < 0) { *err = 1; break; }
						low = low * 16 + h;
					}
					if (*err || low < 0xDC00 || low > 0xDFFF) { *err = 1; break; }
					*pos += 6;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					*err = 1;
					break;
				}
				if (cp < 0x80) gbuf_addch(&b, (char)cp);
				else if (cp < 0x800) {
					gbuf_addch(&b, (char)(0xC0 | (cp >> 6)));
					gbuf_addch(&b, (char)(0x80 | (cp & 0x3F)));
				} else if (cp < 0x10000) {
					gbuf_addch(&b, (char)(0xE0 | (cp >> 12)));
					gbuf_addch(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
					gbuf_addch(&b, (char)(0x80 | (cp & 0x3F)));
				} else {
					gbuf_addch(&b, (char)(0xF0 | (cp >> 18)));
					gbuf_addch(&b, (char)(0x80 | ((cp >> 12) & 0x3F)));
					gbuf_addch(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
					gbuf_addch(&b, (char)(0x80 | (cp & 0x3F)));
				}
				break;
			}
			default: *err = 1;
			}
			if (*err) break;
		} else if ((unsigned char)c < 0x20) {
			*err = 1;
			break;
		} else {
			gbuf_addch(&b, c);
			(*pos)++;
		}
	}
	gbuf_free(&b);
	return NULL;
}

static jv *parse_value(const char *s, size_t len, size_t *pos, int *err) {
	skipws(s, len, pos);
	if (*pos >= len) { *err = 1; return NULL; }
	size_t start = *pos;
	char c = s[*pos];

	if (c == '"') {
		jv *v = parse_string(s, len, pos, err);
		if (v) { v->raw = s + start; v->rawlen = *pos - start; }
		return v;
	}
	if (c == '{') {
		(*pos)++;
		jv *v = jv_new(5);
		if (!v) { *err = 1; return NULL; }
		skipws(s, len, pos);
		if (*pos < len && s[*pos] == '}') {
			(*pos)++;
			v->raw = s + start; v->rawlen = *pos - start;
			return v;
		}
		for (;;) {
			skipws(s, len, pos);
			jv *k = parse_string(s, len, pos, err);
			if (!k) { jv_free(v); return NULL; }
			skipws(s, len, pos);
			if (*pos >= len || s[*pos] != ':') { *err = 1; jv_free(v); free(k); return NULL; }
			(*pos)++;
			jv *val = parse_value(s, len, pos, err);
			if (!val) { *err = 1; jv_free(v); free(k); return NULL; }
			if (!jv_grow_kv(v)) { *err = 1; jv_free(v); free(k->str); free(k); jv_free(val); return NULL; }
			v->kv[v->nk].k = k->str;
			v->kv[v->nk].v = val;
			v->nk++;
			free(k);
			skipws(s, len, pos);
			if (*pos >= len) { *err = 1; jv_free(v); return NULL; }
			if (s[*pos] == ',') { (*pos)++; continue; }
			if (s[*pos] == '}') {
				(*pos)++;
				v->raw = s + start; v->rawlen = *pos - start;
				return v;
			}
			*err = 1;
			jv_free(v);
			return NULL;
		}
	}
	if (c == '[') {
		(*pos)++;
		jv *v = jv_new(4);
		if (!v) { *err = 1; return NULL; }
		skipws(s, len, pos);
		if (*pos < len && s[*pos] == ']') {
			(*pos)++;
			v->raw = s + start; v->rawlen = *pos - start;
			return v;
		}
		for (;;) {
			jv *val = parse_value(s, len, pos, err);
			if (!val) { jv_free(v); return NULL; }
			if (!jv_grow_items(v)) { *err = 1; jv_free(v); return NULL; }
			v->items[v->ni++] = val;
			skipws(s, len, pos);
			if (*pos >= len) { *err = 1; jv_free(v); return NULL; }
			if (s[*pos] == ',') { (*pos)++; continue; }
			if (s[*pos] == ']') {
				(*pos)++;
				v->raw = s + start; v->rawlen = *pos - start;
				return v;
			}
			*err = 1;
			jv_free(v);
			return NULL;
		}
	}
	if (c == 't') {
		if (len - *pos >= 4 && memcmp(s + *pos, "true", 4) == 0) {
			*pos += 4;
			jv *v = jv_new(1); if (!v) { *err = 1; return NULL; } v->b = 1;
			v->raw = s + start; v->rawlen = 4;
			return v;
		}
		*err = 1; return NULL;
	}
	if (c == 'f') {
		if (len - *pos >= 5 && memcmp(s + *pos, "false", 5) == 0) {
			*pos += 5;
			jv *v = jv_new(1); if (!v) { *err = 1; return NULL; } v->b = 0;
			v->raw = s + start; v->rawlen = 5;
			return v;
		}
		*err = 1; return NULL;
	}
	if (c == 'n') {
		if (len - *pos >= 4 && memcmp(s + *pos, "null", 4) == 0) {
			*pos += 4;
			jv *v = jv_new(0); if (!v) { *err = 1; return NULL; }
			v->raw = s + start; v->rawlen = 4;
			return v;
		}
		*err = 1; return NULL;
	}
	/* number */
	{
		size_t p = *pos;
		if (s[p] == '-') p++;
		int digits = 0;
		if (p < len && s[p] == '0') {
			p++;
			digits = 1;
			if (p < len && isdigit((unsigned char)s[p])) { *err = 1; return NULL; }
		} else {
			while (p < len && isdigit((unsigned char)s[p])) { p++; digits++; }
		}
		if (p < len && s[p] == '.') {
			p++;
			size_t frac = p;
			while (p < len && isdigit((unsigned char)s[p])) p++;
			if (p == frac) { *err = 1; return NULL; }
		}
		if (p < len && (s[p] == 'e' || s[p] == 'E')) {
			p++;
			if (p < len && (s[p] == '+' || s[p] == '-')) p++;
			size_t exp = p;
			while (p < len && isdigit((unsigned char)s[p])) p++;
			if (p == exp) { *err = 1; return NULL; }
		}
		if (digits == 0) { *err = 1; return NULL; }
		jv *v = jv_new(2);
		if (!v) { *err = 1; return NULL; }
		char tmp[64];
		size_t tl = p - *pos;
		size_t tmp_len = tl < sizeof tmp ? tl : sizeof tmp - 1;
		memcpy(tmp, s + *pos, tmp_len);
		tmp[tmp_len] = 0;
		v->num = strtod(tmp, NULL);
		v->raw = s + *pos;
		v->rawlen = p - *pos;
		*pos = p;
		return v;
	}
}

jv *json_parse(const char *s, size_t len) {
	if (!s || !utf8_valid(s, len)) return NULL;
	size_t pos = 0;
	int err = 0;
	jv *v = parse_value(s, len, &pos, &err);
	if (err || !v) {
		if (v) jv_free(v);
		return NULL;
	}
	skipws(s, len, &pos);
	if (pos < len) {
		jv_free(v);
		return NULL;
	}
	return v;
}

jv *jv_get(jv *obj, const char *key) {
	if (!obj || obj->t != 5) return NULL;
	for (int i = 0; i < obj->nk; i++)
		if (strcmp(obj->kv[i].k, key) == 0) return obj->kv[i].v;
	return NULL;
}

const char *jv_str(jv *v) { return (v && v->t == 3) ? v->str : NULL; }

void jv_free(jv *v) {
	if (!v) return;
	if (v->t == 3) free(v->str);
	if (v->t == 4) {
		for (int i = 0; i < v->ni; i++) jv_free(v->items[i]);
		free(v->items);
	}
	if (v->t == 5) {
		for (int i = 0; i < v->nk; i++) {
			free(v->kv[i].k);
			jv_free(v->kv[i].v);
		}
		free(v->kv);
	}
	free(v);
}

void json_escape(gbuf *b, const char *s, size_t len) {
	gbuf_addch(b, '"');
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"':  gbuf_adds(b, "\\\""); break;
		case '\\': gbuf_adds(b, "\\\\"); break;
		case '\n': gbuf_adds(b, "\\n"); break;
		case '\r': gbuf_adds(b, "\\r"); break;
		case '\t': gbuf_adds(b, "\\t"); break;
		case '\b': gbuf_adds(b, "\\b"); break;
		case '\f': gbuf_adds(b, "\\f"); break;
		default:
			if (c < 0x20) {
				char t[8];
				snprintf(t, sizeof t, "\\u%04x", c);
				gbuf_adds(b, t);
			} else if (c >= 0x80) {
				size_t n = utf8_sequence((const unsigned char *)s + i, len - i);
				if (n) {
					gbuf_addn(b, s + i, n);
					i += n - 1;
				} else {
					char t[8];
					snprintf(t, sizeof t, "\\u%04x", c);
					gbuf_adds(b, t);
				}
			} else {
				gbuf_addch(b, (char)c);
			}
		}
	}
	gbuf_addch(b, '"');
}
