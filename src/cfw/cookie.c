/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cookie.h>

#include <cfw/http.h>

#include <stdio.h>
#include <string.h>

/* RFC 9110 token characters (tchar). */
static int cfw_cookie_is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return 1;
    default:
        return 0;
    }
}

/* RFC 6265 cookie-octet: visible ASCII except DQUOTE, comma,
 * semicolon, and backslash. */
static int cfw_cookie_is_cookie_octet(unsigned char c)
{
    if (c == 0x21 || (c >= 0x23 && c <= 0x2b) ||
        (c >= 0x2d && c <= 0x3a) || (c >= 0x3c && c <= 0x5b) ||
        (c >= 0x5d && c <= 0x7e)) {
        return 1;
    }
    return 0;
}

static int cfw_cookie_is_ows(unsigned char c)
{
    return c == ' ' || c == '\t';
}

/* Trims leading/trailing OWS from [*start, *end). */
static void cfw_cookie_trim_ows(const char **start, const char **end)
{
    while (*start < *end && cfw_cookie_is_ows((unsigned char)**start)) {
        (*start)++;
    }
    while (*end > *start && cfw_cookie_is_ows((unsigned char)(*end)[-1])) {
        (*end)--;
    }
}

cfw_status_t cfw_cookie_parse(const cfw_strview_t *header,
                              cfw_cookie_pair_t *pairs,
                              size_t pair_capacity,
                              size_t *count_out,
                              size_t *dropped_out)
{
    size_t pos = 0;
    size_t stored = 0;
    size_t dropped = 0;

    if (header == NULL || count_out == NULL ||
        (header->size != 0 && header->data == NULL) ||
        (pair_capacity != 0 && pairs == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *count_out = 0;
    if (dropped_out != NULL) {
        *dropped_out = 0;
    }

    while (pos <= header->size) {
        /* One segment runs to the next ';' or end of input. */
        size_t seg_end = pos;
        const char *name;
        const char *name_end;
        const char *value;
        const char *value_end;
        const char *eq;
        size_t i;
        int name_ok;
        int value_ok;
        cfw_cookie_pair_t *slot;

        while (seg_end < header->size && header->data[seg_end] != ';') {
            seg_end++;
        }
        name = header->data + pos;
        name_end = header->data + seg_end;
        cfw_cookie_trim_ows(&name, &name_end);
        if (name < name_end) {
            /* Split at the first '='; a later '=' stays literal in
             * the value. */
            eq = name;
            while (eq < name_end && *eq != '=') {
                eq++;
            }
            if (eq >= name_end) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            /* Trailing OWS before '=' fails the token check below. */
            name_end = eq;
            value = eq + 1;
            value_end = header->data + seg_end;
            cfw_cookie_trim_ows(&value, &value_end);

            /* Name must be a nonempty token. */
            name_ok = name_end > name;
            for (i = 0; name_ok && i < (size_t)(name_end - name); i++) {
                if (!cfw_cookie_is_tchar((unsigned char)name[i])) {
                    name_ok = 0;
                }
            }
            /* Value must be cookie-octets, optionally DQUOTE-wrapped. */
            value_ok = 1;
            if (value_end - value >= 2 && value[0] == '"' &&
                value[value_end - value - 1] == '"') {
                value++;
                value_end--;
            }
            for (i = 0; value_ok && i < (size_t)(value_end - value);
                 i++) {
                if (!cfw_cookie_is_cookie_octet(
                        (unsigned char)value[i])) {
                    value_ok = 0;
                }
            }
            if (!name_ok || !value_ok) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }

            if (stored < pair_capacity) {
                slot = &pairs[stored];
                cfw_strview_init(&slot->name, name,
                                 (size_t)(name_end - name));
                cfw_strview_init(&slot->value, value,
                                 (size_t)(value_end - value));
                stored++;
            } else {
                dropped++;
            }
        }
        if (seg_end >= header->size) {
            break;
        }
        pos = seg_end + 1;
    }

    *count_out = stored;
    if (dropped_out != NULL) {
        *dropped_out = dropped;
    }
    return CFW_OK;
}

/* Appends `text` (len bytes) to dst at *off; fails when it would not
 * leave room for the NUL terminator. */
static int cfw_cookie_append(char *dst, size_t cap, size_t *off,
                             const char *text, size_t len)
{
    if (len > cap - *off - 1u) {
        return 0;
    }
    memcpy(dst + *off, text, len);
    *off += len;
    return 1;
}

static int cfw_cookie_append_cstr(char *dst, size_t cap, size_t *off,
                                  const char *text)
{
    return cfw_cookie_append(dst, cap, off, text, strlen(text));
}

static int cfw_cookie_append_view(char *dst, size_t cap, size_t *off,
                                  const cfw_strview_t *view)
{
    return cfw_cookie_append(dst, cap, off, view->data, view->size);
}

static int cfw_cookie_valid_token_view(const cfw_strview_t *view)
{
    size_t i;
    if (view->size == 0 ||
        (view->size != 0 && view->data == NULL)) {
        return 0;
    }
    for (i = 0; i < view->size; i++) {
        if (!cfw_cookie_is_tchar((unsigned char)view->data[i])) {
            return 0;
        }
    }
    return 1;
}

static int cfw_cookie_valid_octet_view(const cfw_strview_t *view)
{
    size_t i;
    if (view->size != 0 && view->data == NULL) {
        return 0;
    }
    for (i = 0; i < view->size; i++) {
        if (!cfw_cookie_is_cookie_octet((unsigned char)view->data[i])) {
            return 0;
        }
    }
    return 1;
}

cfw_status_t cfw_cookie_serialize(const cfw_cookie_t *cookie, char *dst,
                                  size_t cap, size_t *written)
{
    size_t off = 0;
    char date[30];
    char num[32];
    int n;

    if (cookie == NULL || dst == NULL || cap == 0 || written == NULL ||
        !cfw_cookie_valid_token_view(&cookie->name) ||
        !cfw_cookie_valid_octet_view(&cookie->value) ||
        (cookie->path.size != 0 &&
         (cookie->path.data == NULL || cookie->path.data[0] != '/' ||
          !cfw_cookie_valid_octet_view(&cookie->path))) ||
        !cfw_cookie_valid_octet_view(&cookie->domain) ||
        (cookie->samesite != CFW_COOKIE_SAMESITE_UNSET &&
         cookie->samesite != CFW_COOKIE_SAMESITE_LAX &&
         cookie->samesite != CFW_COOKIE_SAMESITE_STRICT &&
         cookie->samesite != CFW_COOKIE_SAMESITE_NONE)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (!cfw_cookie_append_view(dst, cap, &off, &cookie->name) ||
        !cfw_cookie_append_cstr(dst, cap, &off, "=") ||
        !cfw_cookie_append_view(dst, cap, &off, &cookie->value)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cookie->path.size != 0 &&
        (!cfw_cookie_append_cstr(dst, cap, &off, "; Path=") ||
         !cfw_cookie_append_view(dst, cap, &off, &cookie->path))) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cookie->domain.size != 0 &&
        (!cfw_cookie_append_cstr(dst, cap, &off, "; Domain=") ||
         !cfw_cookie_append_view(dst, cap, &off, &cookie->domain))) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cookie->max_age_seconds >= 0) {
        n = snprintf(num, sizeof(num), "; Max-Age=%lld",
                     (long long)cookie->max_age_seconds);
        if (n < 0 || (size_t)n >= sizeof(num) ||
            !cfw_cookie_append(dst, cap, &off, num, (size_t)n)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (cookie->expires_unix >= 0) {
        if (cfw_http_date_format(cookie->expires_unix, date,
                                 sizeof(date)) != CFW_OK ||
            !cfw_cookie_append_cstr(dst, cap, &off, "; Expires=") ||
            !cfw_cookie_append(dst, cap, &off, date, 29)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    if (cookie->secure &&
        !cfw_cookie_append_cstr(dst, cap, &off, "; Secure")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cookie->httponly &&
        !cfw_cookie_append_cstr(dst, cap, &off, "; HttpOnly")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cookie->partitioned &&
        !cfw_cookie_append_cstr(dst, cap, &off, "; Partitioned")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    switch (cookie->samesite) {
    case CFW_COOKIE_SAMESITE_LAX:
        if (!cfw_cookie_append_cstr(dst, cap, &off, "; SameSite=Lax")) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        break;
    case CFW_COOKIE_SAMESITE_STRICT:
        if (!cfw_cookie_append_cstr(dst, cap, &off,
                                     "; SameSite=Strict")) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        break;
    case CFW_COOKIE_SAMESITE_NONE:
        if (!cfw_cookie_append_cstr(dst, cap, &off, "; SameSite=None")) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        break;
    case CFW_COOKIE_SAMESITE_UNSET:
    default:
        break;
    }

    dst[off] = '\0';
    *written = off;
    return CFW_OK;
}
