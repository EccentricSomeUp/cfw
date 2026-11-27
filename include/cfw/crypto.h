/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_CRYPTO_H
#define CFW_CRYPTO_H

#include <cfw/export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cryptographic primitives (ADR 0046).
 *
 * SHA-256 and HMAC-SHA256 are provided for integrity tags, message
 * authentication codes (signed cookies, tokens, webhook signatures),
 * and non-adversarial checksums. They are NOT password-hashing
 * functions — use a memory-hard KDF (Argon2id, scrypt, bcrypt) for
 * credential storage, which is out of scope for this dependency-free
 * core.
 *
 * All functions are constant-work per input bit for the compression
 * itself; cfw_secure_equal additionally avoids early-exit so MAC
 * comparisons do not leak timing.
 */

#define CFW_SHA256_DIGEST_SIZE 32u
#define CFW_HMAC_SHA256_DIGEST_SIZE CFW_SHA256_DIGEST_SIZE

typedef struct cfw_sha256_ctx {
    uint32_t state[8];
    uint64_t total_len;      /* message length in bits, mod 2^64 */
    unsigned char block[64]; /* partial block buffer */
    size_t block_len;        /* valid bytes in block */
} cfw_sha256_ctx_t;

CFW_API void cfw_sha256_init(cfw_sha256_ctx_t *ctx);
CFW_API void cfw_sha256_update(cfw_sha256_ctx_t *ctx, const void *data,
                               size_t size);
/* Writes exactly CFW_SHA256_DIGEST_SIZE bytes to `out`. */
CFW_API void cfw_sha256_final(cfw_sha256_ctx_t *ctx,
                              unsigned char *out);
/* One-shot convenience: SHA-256 of data[0..size). */
CFW_API void cfw_sha256(const void *data, size_t size,
                        unsigned char *out);

/* Streaming HMAC-SHA256 (RFC 2104) context for incremental input —
 * large bodies, files, or data that arrives in pieces. The context
 * is ~200 bytes; keep it on the stack or in caller-owned storage.
 * Not internally synchronized. */
typedef struct cfw_hmac_sha256_ctx {
    cfw_sha256_ctx_t inner;
    unsigned char kblock[64]; /* inner-padded key block */
} cfw_hmac_sha256_ctx_t;

/* Initializes the context with `key` (any length; longer keys are
 * hashed first per RFC 2104). Same NULL/size contract as
 * cfw_hmac_sha256: invalid arguments leave the context unusable and
 * the call is a no-op. */
CFW_API void cfw_hmac_sha256_init(cfw_hmac_sha256_ctx_t *ctx,
                                  const void *key, size_t key_size);
CFW_API void cfw_hmac_sha256_update(cfw_hmac_sha256_ctx_t *ctx,
                                    const void *data, size_t size);
/* Writes CFW_HMAC_SHA256_DIGEST_SIZE bytes to `out` and invalidates
 * the context (it must be re-init'ed for reuse). */
CFW_API void cfw_hmac_sha256_final(cfw_hmac_sha256_ctx_t *ctx,
                                   unsigned char *out);

/* HMAC-SHA256 (RFC 2104) with the standard 64-byte block size.
 * `key` may be any length (longer keys are hashed first, per spec).
 * Writes CFW_HMAC_SHA256_DIGEST_SIZE bytes to `out`. */
CFW_API void cfw_hmac_sha256(const void *key, size_t key_size,
                             const void *data, size_t data_size,
                             unsigned char *out);

/* Constant-time equality for `size` bytes at a and b. Returns nonzero
 * iff all bytes are equal. Callers compare lengths separately; the
 * function itself does not early-exit on mismatch. NULL with size 0
 * is valid; NULL with nonzero size is undefined behavior (callers
 * must validate pointers). */
CFW_API int cfw_secure_equal(const void *a, const void *b, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CFW_CRYPTO_H */
