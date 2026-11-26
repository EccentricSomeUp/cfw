/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/signed.h>

#include <cfw/codec.h>
#include <cfw/crypto.h>

#include <string.h>

/* Wire format: "v1." + base64(payload) + "." + base64(HMAC-SHA256
 * over "v1." + base64(payload)). The MAC covers the literal encoded
 * prefix, so verification needs no re-encoding. */

#define CFW_SIGNED_PREFIX "v1."
#define CFW_SIGNED_PREFIX_LEN 3u
#define CFW_SIGNED_MAC_B64_LEN 44u /* canonical base64 of 32 bytes */
#define CFW_SIGNED_MAC_LEN CFW_SHA256_DIGEST_SIZE

size_t cfw_signed_encode_bound(size_t payload_size)
{
    size_t b64;

    b64 = cfw_base64_encode_bound(payload_size);
    if (b64 == SIZE_MAX) {
        return SIZE_MAX;
    }
    /* "v1." + b64 chars (bound includes NUL; subtract it) + "." +
     * 44 MAC chars + NUL. */
    if (b64 - 1u > SIZE_MAX - CFW_SIGNED_PREFIX_LEN - 1u -
                           CFW_SIGNED_MAC_B64_LEN - 1u) {
        return SIZE_MAX;
    }
    return CFW_SIGNED_PREFIX_LEN + (b64 - 1u) + 1u +
           CFW_SIGNED_MAC_B64_LEN + 1u;
}

cfw_status_t cfw_signed_encode(const void *key, size_t key_size,
                               const void *payload, size_t payload_size,
                               char *out, size_t cap, size_t *written)
{
    unsigned char mac[CFW_SIGNED_MAC_LEN];
    size_t p64_len = 0;
    size_t mac_len = 0;
    size_t total;
    cfw_status_t st;

    if (out == NULL || (key_size != 0 && key == NULL) ||
        (payload_size != 0 && payload == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    total = cfw_signed_encode_bound(payload_size);
    if (total == SIZE_MAX || cap < total) {
        return CFW_ERROR_OVERFLOW;
    }

    /* "v1." + base64(payload); the base64 encoder NUL-terminates
     * after the payload segment, which the '.' below overwrites. */
    memcpy(out, CFW_SIGNED_PREFIX, CFW_SIGNED_PREFIX_LEN);
    st = cfw_base64_encode(payload, payload_size, out + CFW_SIGNED_PREFIX_LEN,
                           cap - CFW_SIGNED_PREFIX_LEN, &p64_len);
    if (st != CFW_OK) {
        return st;
    }

    /* MAC over the literal "v1." + base64(payload) span. */
    cfw_hmac_sha256(key, key_size, out, CFW_SIGNED_PREFIX_LEN + p64_len,
                    mac);

    out[CFW_SIGNED_PREFIX_LEN + p64_len] = '.';
    st = cfw_base64_encode(mac, sizeof(mac),
                           out + CFW_SIGNED_PREFIX_LEN + p64_len + 1u,
                           cap - CFW_SIGNED_PREFIX_LEN - p64_len - 1u,
                           &mac_len);
    if (st != CFW_OK) {
        return st;
    }
    if (written != NULL) {
        *written = CFW_SIGNED_PREFIX_LEN + p64_len + 1u + mac_len;
    }
    return CFW_OK;
}

cfw_status_t cfw_signed_decode(const void *key, size_t key_size,
                               const cfw_strview_t *token,
                               unsigned char *payload_out,
                               size_t payload_cap, size_t *payload_size)
{
    const char *data;
    size_t size;
    size_t sep;
    size_t p64_len;
    size_t mac_b64_len;
    unsigned char mac_expected[CFW_SIGNED_MAC_LEN];
    unsigned char mac_actual[CFW_SIGNED_MAC_LEN];
    size_t mac_actual_len = 0;
    cfw_strview_t p64;
    cfw_strview_t mac_view;
    size_t decoded = 0;

    if (key_size != 0 && key == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (token == NULL || (token->size != 0 && token->data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    data = token->data;
    size = token->size;

    /* Structural parse: "v1." + <b64> + "." + <b64mac>. The encoded
     * segments contain no '.', so the first '.' after the prefix is
     * the separator and the remainder is the MAC segment. */
    if (size < CFW_SIGNED_PREFIX_LEN + 1u + CFW_SIGNED_MAC_B64_LEN ||
        memcmp(data, CFW_SIGNED_PREFIX, CFW_SIGNED_PREFIX_LEN) != 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    sep = CFW_SIGNED_PREFIX_LEN;
    while (sep < size && data[sep] != '.') {
        sep++;
    }
    if (sep == size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    p64_len = sep - CFW_SIGNED_PREFIX_LEN;
    mac_b64_len = size - sep - 1u;
    if (mac_b64_len != CFW_SIGNED_MAC_B64_LEN) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Decode the MAC segment first; a wrong-length or malformed MAC
     * fails before touching the payload buffer. */
    mac_view.data = data + sep + 1u;
    mac_view.size = mac_b64_len;
    if (cfw_base64_decode(&mac_view, mac_actual, sizeof(mac_actual),
                          &mac_actual_len) != CFW_OK ||
        mac_actual_len != sizeof(mac_actual)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Recompute the MAC over the literal "v1."+b64 prefix and
     * compare in constant time. */
    cfw_hmac_sha256(key, key_size, data, sep, mac_expected);
    if (!cfw_secure_equal(mac_expected, mac_actual, sizeof(mac_expected))) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Only now decode the payload. */
    p64.data = data + CFW_SIGNED_PREFIX_LEN;
    p64.size = p64_len;
    if (payload_out == NULL && p64.size != 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    {
        cfw_status_t st =
            cfw_base64_decode(&p64, payload_out, payload_cap, &decoded);
        if (st == CFW_ERROR_OVERFLOW) {
            return CFW_ERROR_OVERFLOW;
        }
        if (st != CFW_OK) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (payload_size != NULL) {
        *payload_size = decoded;
    }
    return CFW_OK;
}
