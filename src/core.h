#ifndef SBM_CORE_H
#define SBM_CORE_H

#include <stddef.h>

/* Growable byte buffer. Any failed allocation sets failed and freezes
 * the buffer: later appends are no-ops, so callers check once at the
 * end instead of after every append. */
typedef struct {
	char *p;
	size_t len;
	size_t cap;
	int failed;
} gbuf;

void gbuf_init(gbuf *b);
void gbuf_free(gbuf *b);
void gbuf_addn(gbuf *b, const char *s, size_t n);
void gbuf_adds(gbuf *b, const char *s);
void gbuf_addch(gbuf *b, char c);

#endif
