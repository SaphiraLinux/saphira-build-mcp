#ifndef AKAMAN_JSON_H
#define AKAMAN_JSON_H

#include "core.h"

typedef struct jv jv;
struct jv {
	int t; /* 0 null, 1 bool, 2 num, 3 str, 4 arr, 5 obj */
	int b;
	double num;
	char *str;
	jv **items;
	int ni, cap;
	struct {
		char *k;
		jv *v;
	} *kv;
	int nk, kcap;
	const char *raw;
	size_t rawlen;
};

jv *json_parse(const char *s, size_t len);
jv *jv_get(jv *obj, const char *key);
const char *jv_str(jv *v);
void jv_free(jv *v);
void json_escape(gbuf *b, const char *s, size_t len);

#endif
