#ifndef SBM_SHA256_H
#define SBM_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Self-contained SHA-256 (FIPS 180-4), written for this repository
 * so evidence digests never depend on an external hasher: the
 * previous /bin/sha256sum frontend silently failed on hosts
 * without coreutils (macOS), pinning every freshness check to a
 * digest that could never move. */

typedef struct {
	uint32_t h[8];
	uint64_t len;
	unsigned char buf[64];
	size_t buflen;
} sb_sha256;

void sb_sha256_init(sb_sha256 *c);
void sb_sha256_update(sb_sha256 *c, const void *data, size_t n);
void sb_sha256_final(sb_sha256 *c, unsigned char out[32]);

/* Hex digest of one file. Returns 0 with 64 lowercase hex chars in
 * hex[65]; -1 when the file cannot be read. */
int sb_sha256_file(const char *path, char hex[65]);

#endif
