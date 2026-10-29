#include "sha256.h"

#include <stdio.h>
#include <string.h>

static uint32_t ror(uint32_t x, int n) {
	return (x >> n) | (x << (32 - n));
}

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void block(sb_sha256 *s, const unsigned char *p) {
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[4 * i] << 24) |
		       ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) |
		       (uint32_t)p[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^
		              (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^
		              (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	f = s->h[5];
	g = s->h[6];
	h = s->h[7];
	for (i = 0; i < 64; i++) {
		uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		t1 = h + S1 + ch + K[i] + w[i];
		t2 = S0 + mj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
	s->h[5] += f;
	s->h[6] += g;
	s->h[7] += h;
}

void sb_sha256_init(sb_sha256 *c) {
	c->h[0] = 0x6a09e667;
	c->h[1] = 0xbb67ae85;
	c->h[2] = 0x3c6ef372;
	c->h[3] = 0xa54ff53a;
	c->h[4] = 0x510e527f;
	c->h[5] = 0x9b05688c;
	c->h[6] = 0x1f83d9ab;
	c->h[7] = 0x5be0cd19;
	c->len = 0;
	c->buflen = 0;
}

void sb_sha256_update(sb_sha256 *c, const void *data, size_t n) {
	const unsigned char *p = data;
	c->len += (uint64_t)n;
	while (n > 0) {
		size_t take = 64 - c->buflen;
		if (take > n)
			take = n;
		memcpy(c->buf + c->buflen, p, take);
		c->buflen += take;
		p += take;
		n -= take;
		if (c->buflen == 64) {
			block(c, c->buf);
			c->buflen = 0;
		}
	}
}

void sb_sha256_final(sb_sha256 *c, unsigned char out[32]) {
	uint64_t bits = c->len * 8;
	unsigned char pad[64];
	int i;
	pad[0] = 0x80;
	memset(pad + 1, 0, sizeof pad - 1);
	/* Enough room for 0x80 plus length, else one extra block. */
	if (c->buflen < 56)
		sb_sha256_update(c, pad, 56 - c->buflen);
	else {
		sb_sha256_update(c, pad, 64 - c->buflen);
		sb_sha256_update(c, pad, 56);
	}
	/* c->buflen is 56 here by construction; the length goes in
	 * the last 8 bytes of the final block. */
	for (i = 0; i < 8; i++)
		c->buf[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
	block(c, c->buf);
	for (i = 0; i < 8; i++) {
		out[4 * i] = (unsigned char)(c->h[i] >> 24);
		out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
		out[4 * i + 3] = (unsigned char)c->h[i];
	}
}

int sb_sha256_file(const char *path, char hex[65]) {
	static const char *digits = "0123456789abcdef";
	unsigned char sum[32];
	sb_sha256 c;
	FILE *f;
	unsigned char buf[8192];
	size_t n;
	int i;
	if (!path)
		return -1;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	sb_sha256_init(&c);
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		sb_sha256_update(&c, buf, n);
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	sb_sha256_final(&c, sum);
	for (i = 0; i < 32; i++) {
		hex[2 * i] = digits[sum[i] >> 4];
		hex[2 * i + 1] = digits[sum[i] & 15];
	}
	hex[64] = 0;
	return 0;
}
