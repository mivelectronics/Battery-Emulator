/*
 * SHA-256 (FIPS 180-4) for checking an update image. Small and plain: the image check runs once per update.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_SHA256_H
#define BE_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct be_sha256 {
	uint32_t h[8];
	uint64_t len; /* bytes so far */
	uint8_t buf[64];
	uint8_t fill;
};

void be_sha256_init(struct be_sha256 *s);
void be_sha256_update(struct be_sha256 *s, const void *data, size_t len);
void be_sha256_final(struct be_sha256 *s, uint8_t out[32]);

#endif /* BE_SHA256_H */
