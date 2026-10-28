#include "core.h"

#include <stdlib.h>
#include <string.h>

void gbuf_init(gbuf *b) {
	b->p = NULL;
	b->len = 0;
	b->cap = 0;
	b->failed = 0;
}

void gbuf_free(gbuf *b) {
	free(b->p);
	gbuf_init(b);
}

static int grow(gbuf *b, size_t need) {
	size_t want, nc;
	char *p;

	if (b->failed)
		return 0;
	if (need > (size_t)-1 - b->len) {
		b->failed = 1;
		return 0;
	}
	want = b->len + need + 1;
	if (want <= b->cap)
		return 1;
	nc = b->cap ? b->cap : 64;
	while (nc < want) {
		if (nc > ((size_t)-1) / 2) {
			nc = want;
			break;
		}
		nc *= 2;
	}
	p = realloc(b->p, nc);
	if (!p) {
		b->failed = 1;
		return 0;
	}
	b->p = p;
	b->cap = nc;
	return 1;
}

void gbuf_addn(gbuf *b, const char *s, size_t n) {
	if (!n || b->failed)
		return;
	if (!grow(b, n))
		return;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

void gbuf_adds(gbuf *b, const char *s) {
	gbuf_addn(b, s, strlen(s));
}

void gbuf_addch(gbuf *b, char c) {
	gbuf_addn(b, &c, 1);
}
