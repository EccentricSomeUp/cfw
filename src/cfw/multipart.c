/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/multipart.h>

#include <cfw/http.h>

#include <string.h>

/* RFC 2046: boundary is 0-69 bchars plus a final bcharsnospace,
 * capped at 70 octets. */
#define CFW_MP_MAX_BOUNDARY 70u

static int cfw_mp_is_ows(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static void cfw_mp_trim(const unsigned char **data, size_t *size)
{
    while (*size != 0 && cfw_mp_is_ows(**data)) {
        (*data)++;
        (*size)--;
    }
    while (*size != 0 && cfw_mp_is_ows((*data)[*size - 1])) {
        (*size)--;
    }
}

static int cfw_mp_ieq(const unsigned char *a, size_t alen,
                      const unsigned char *b, size_t blen)
{
    size_t i;
    if (alen != blen) {
        return 0;
    }
    for (i = 0; i < alen; i++) {
        unsigned char ca = a[i];
        unsigned char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/* Iterates the ';'-separated parameter segments of a header value,
 * skipping segments without '='. On each call fills pname/pvalue
 * with trimmed views (value still quoted when quoted) and returns
 * CFW_OK; *done is set when no name=value segment remains. The first
 * segment (the type/subtype or disposition type) is skipped by the
 * caller via an initial cursor past the first ';'. */
static cfw_status_t cfw_mp_next_param(const cfw_strview_t *value,
                                      size_t *cursor, cfw_strview_t *pname,
                                      cfw_strview_t *pvalue, int *done)
{
    const unsigned char *data = (const unsigned char *)value->data;
    size_t size = value->size;

    *done = 0;
    for (;;) {
        size_t pos;
        size_t end;
        size_t eq;
        const unsigned char *seg;
        size_t seglen;

        if (*cursor >= size) {
            *done = 1;
            return CFW_OK;
        }
        pos = *cursor;
        end = pos;
        while (end < size && data[end] != ';') {
            end++;
        }
        *cursor = end < size ? end + 1 : end;

        seg = data + pos;
        seglen = end - pos;
        cfw_mp_trim(&seg, &seglen);
        eq = 0;
        while (eq < seglen && seg[eq] != '=') {
            eq++;
        }
        if (eq == seglen) {
            continue; /* bare token segment: skip */
        }
        {
            const unsigned char *n = seg;
            size_t nl = eq;
            const unsigned char *v = seg + eq + 1;
            size_t vl = seglen - eq - 1;
            cfw_mp_trim(&n, &nl);
            cfw_mp_trim(&v, &vl);
            cfw_strview_init(pname, n, nl);
            cfw_strview_init(pvalue, v, vl);
        }
        return CFW_OK;
    }
}

/* Unquotes a parameter value: '"..."' yields the inner bytes (with
 * quoted-pair unescaping into *scratch_out when a backslash is
 * present), anything else is the trimmed token itself. Fails on an
 * unterminated or malformed quoted-string. */
static cfw_status_t cfw_mp_param_value(const cfw_strview_t *raw,
                                       cfw_strview_t *out,
                                       char **scratch_out)
{
    const unsigned char *d = (const unsigned char *)raw->data;
    size_t n = raw->size;

    if (n >= 2 && d[0] == '"') {
        size_t inner_end;
        size_t i;
        int needs_unescape = 0;
        if (d[n - 1] != '"') {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        inner_end = n - 1;
        for (i = 1; i < inner_end; i++) {
            if (d[i] == '"' || d[i] == '\\' ||
                (unsigned char)d[i] < 0x20 ||
                (unsigned char)d[i] == 0x7f) {
                if (d[i] == '\\' && i + 1 < inner_end) {
                    needs_unescape = 1;
                    i++;
                    continue;
                }
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!needs_unescape) {
            /* Quoted but no escapes: reference the inner bytes. */
            return cfw_strview_init(out, d + 1, inner_end - 1);
        }
        {
            char *w = *scratch_out;
            for (i = 1; i < inner_end; i++) {
                if (d[i] == '\\' && i + 1 < inner_end) {
                    i++;
                }
                *w++ = (char)d[i];
            }
            cfw_strview_init(out, *scratch_out,
                             (size_t)(w - *scratch_out));
            *scratch_out = w;
        }
        return CFW_OK;
    }
    /* Bare token: reject CTLs/whitespace inside. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = d[i];
        if (c <= 0x20 || c == 0x7f) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    return cfw_strview_init(out, raw->data, n);
}

/* Finds "--" + boundary starting the search at `from`. Returns the
 * index of the first '-' or SIZE_MAX. Does not validate what follows
 * the boundary; callers do. */
static size_t cfw_mp_find_delim(const cfw_span_t *body,
                                const cfw_strview_t *boundary,
                                size_t from, int need_crlf)
{
    const uint8_t *b = body->data;
    size_t n = body->size;
    size_t blen = boundary->size;
    size_t pos;

    if (blen + 2 + (need_crlf ? 2u : 0u) > n || from > n) {
        return (size_t)-1;
    }
    pos = from;
    while (pos + blen + 2 + (need_crlf ? 2u : 0u) <= n) {
        size_t i;
        int match = 1;
        if (need_crlf) {
            if (b[pos] != '\r' || b[pos + 1] != '\n' ||
                b[pos + 2] != '-' || b[pos + 3] != '-') {
                pos++;
                continue;
            }
            i = pos + 4;
        } else {
            if (b[pos] != '-' || b[pos + 1] != '-') {
                pos++;
                continue;
            }
            i = pos + 2;
        }
        for (size_t k = 0; k < blen; k++) {
            if (b[i + k] != (uint8_t)boundary->data[k]) {
                match = 0;
                break;
            }
        }
        if (match) {
            return need_crlf ? pos + 2 : pos;
        }
        pos++;
    }
    return (size_t)-1;
}

/* After a boundary match ending at index `pos` (exclusive of
 * boundary), validates the delimiter trailer: transport padding then
 * either "--" (close) or CRLF (open). Returns 1 for close, 0 for
 * open (with *next set past the CRLF), -1 when malformed. */
static int cfw_mp_delim_trailer(const cfw_span_t *body, size_t pos,
                                size_t *next)
{
    const uint8_t *b = body->data;
    size_t n = body->size;

    /* Transport padding. */
    while (pos < n && (b[pos] == ' ' || b[pos] == '\t')) {
        pos++;
    }
    if (pos + 1 < n && b[pos] == '-' && b[pos + 1] == '-') {
        /* Close delimiter: padding then CRLF or end of body. */
        pos += 2;
        while (pos < n && (b[pos] == ' ' || b[pos] == '\t')) {
            pos++;
        }
        if (pos == n) {
            *next = pos;
            return 1;
        }
        if (pos + 1 < n && b[pos] == '\r' && b[pos + 1] == '\n') {
            *next = pos + 2;
            return 1;
        }
        return -1;
    }
    if (pos + 1 < n && b[pos] == '\r' && b[pos + 1] == '\n') {
        *next = pos + 2;
        return 0;
    }
    return -1;
}

cfw_status_t cfw_multipart_boundary(const cfw_strview_t *content_type,
                                    cfw_strview_t *boundary)
{
    cfw_strview_t cursor_sv;
    size_t cursor = 0;
    int done = 0;

    if (content_type == NULL || boundary == NULL ||
        (content_type->size != 0 && content_type->data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    cursor_sv = *content_type;
    /* Skip the type/subtype segment (up to the first ';'). */
    {
        size_t i = 0;
        while (i < cursor_sv.size &&
               ((const unsigned char *)cursor_sv.data)[i] != ';') {
            i++;
        }
        cursor = i < cursor_sv.size ? i + 1 : i;
    }
    for (;;) {
        cfw_strview_t pname;
        cfw_strview_t pvalue;
        cfw_status_t st;
        st = cfw_mp_next_param(&cursor_sv, &cursor, &pname, &pvalue,
                               &done);
        if (st != CFW_OK) {
            return st;
        }
        if (done) {
            break;
        }
        {
            static const char bn[] = "boundary";
            cfw_strview_t bnv;
            cfw_strview_init(&bnv, bn, sizeof(bn) - 1);
            if (!cfw_mp_ieq((const unsigned char *)pname.data,
                            pname.size,
                            (const unsigned char *)bnv.data,
                            bnv.size)) {
                continue;
            }
        }
        /* Found it: value must be a token or a simple quoted-string
         * without escapes. */
        {
            const unsigned char *d =
                (const unsigned char *)pvalue.data;
            size_t n = pvalue.size;
            if (n >= 2 && d[0] == '"' && d[n - 1] == '"') {
                size_t inner = n - 2;
                size_t i;
                for (i = 0; i < inner; i++) {
                    unsigned char c = d[1 + i];
                    if (c == '"' || c == '\\' || c < 0x20 ||
                        c == 0x7f) {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                }
                if (inner == 0 || inner > CFW_MP_MAX_BOUNDARY) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                return cfw_strview_init(boundary, d + 1, inner);
            }
            /* Bare token. */
            {
                size_t i;
                if (n == 0 || n > CFW_MP_MAX_BOUNDARY) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                for (i = 0; i < n; i++) {
                    unsigned char c = d[i];
                    if (c <= 0x20 || c == 0x7f || c == '"' ||
                        c == ';' || c == '\\') {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                }
                return cfw_strview_init(boundary, pvalue.data, n);
            }
        }
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}

/* Extracts name/filename from a Content-Disposition header value into
 * the part struct, unescaping into *scratch_out. */
static cfw_status_t cfw_mp_disposition(const cfw_strview_t *value,
                                       cfw_multipart_part_t *part,
                                       char **scratch_out)
{
    cfw_strview_t cursor_sv = *value;
    size_t cursor = 0;
    int done = 0;

    /* Skip the disposition type segment. */
    {
        size_t i = 0;
        while (i < cursor_sv.size &&
               ((const unsigned char *)cursor_sv.data)[i] != ';') {
            i++;
        }
        cursor = i < cursor_sv.size ? i + 1 : i;
    }
    for (;;) {
        cfw_strview_t pname;
        cfw_strview_t pvalue;
        cfw_strview_t decoded;
        cfw_status_t st;
        st = cfw_mp_next_param(&cursor_sv, &cursor, &pname, &pvalue,
                               &done);
        if (st != CFW_OK) {
            return st;
        }
        if (done) {
            break;
        }
        {
            static const char nn[] = "name";
            static const char fn[] = "filename";
            cfw_strview_t nnv;
            cfw_strview_t fnv;
            cfw_strview_init(&nnv, nn, sizeof(nn) - 1);
            cfw_strview_init(&fnv, fn, sizeof(fn) - 1);
            if (cfw_mp_ieq((const unsigned char *)pname.data,
                           pname.size,
                           (const unsigned char *)nnv.data, nnv.size)) {
                st = cfw_mp_param_value(&pvalue, &decoded,
                                        scratch_out);
                if (st != CFW_OK) {
                    return st;
                }
                part->name = decoded;
            } else if (cfw_mp_ieq(
                           (const unsigned char *)pname.data,
                           pname.size,
                           (const unsigned char *)fnv.data,
                           fnv.size)) {
                st = cfw_mp_param_value(&pvalue, &decoded,
                                        scratch_out);
                if (st != CFW_OK) {
                    return st;
                }
                part->filename = decoded;
            }
        }
    }
    return CFW_OK;
}

cfw_status_t cfw_multipart_parse(
    const cfw_span_t *body, const cfw_strview_t *boundary,
    cfw_multipart_part_t *parts, size_t part_capacity, char *scratch,
    size_t scratch_size, size_t *count_out, size_t *dropped_out)
{
    size_t pos;
    size_t stored = 0;
    size_t dropped = 0;
    char *out;

    if (body == NULL || boundary == NULL || count_out == NULL ||
        (body->size != 0 && body->data == NULL) ||
        boundary->size == 0 ||
        (part_capacity != 0 && parts == NULL) ||
        (scratch_size != 0 && scratch == NULL) ||
        scratch_size < body->size) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *count_out = 0;
    if (dropped_out != NULL) {
        *dropped_out = 0;
    }
    if (body->size == 0) {
        return CFW_OK;
    }
    out = scratch;

    /* First delimiter: no leading CRLF required (preamble allowed).
     * Candidate matches whose trailer is not a valid delimiter
     * continuation (boundary as a prefix of a longer token) are
     * skipped. */
    {
        size_t d = cfw_mp_find_delim(body, boundary, 0, 0);
        int trailer = -1;
        size_t next = 0;
        while (d != (size_t)-1) {
            trailer = cfw_mp_delim_trailer(body,
                                           d + boundary->size + 2,
                                           &next);
            if (trailer >= 0) {
                break;
            }
            d = cfw_mp_find_delim(body, boundary, d + 1, 0);
        }
        if (d == (size_t)-1) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (trailer == 1) {
            /* Immediate close: zero parts is legal. */
            return CFW_OK;
        }
        pos = next;
    }

    for (;;) {
        /* Header block: either an immediate blank line (no headers)
         * or lines up to CRLF CRLF. */
        const uint8_t *b = body->data;
        size_t h_start = pos;
        size_t h_end;
        size_t body_start;
        cfw_multipart_part_t part;
        cfw_strview_t hb;
        cfw_strview_t cursor;
        size_t hcursor = 0;
        int hdone = 0;
        size_t dpos;
        int trailer;
        size_t next;
        cfw_status_t st;

        memset(&part, 0, sizeof(part));

        if (h_start + 1 < body->size && b[h_start] == '\r' &&
            b[h_start + 1] == '\n') {
            h_end = h_start;
            body_start = h_start + 2;
        } else {
            /* Find CRLF CRLF. */
            h_end = (size_t)-1;
            {
                size_t i = h_start;
                while (i + 3 < body->size) {
                    if (b[i] == '\r' && b[i + 1] == '\n' &&
                        b[i + 2] == '\r' && b[i + 3] == '\n') {
                        h_end = i;
                        break;
                    }
                    i++;
                }
            }
            if (h_end == (size_t)-1) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            /* header block includes through the blank line's CRLF. */
            body_start = h_end + 4;
            hb.data = (const char *)(b + h_start);
            hb.size = h_end + 4 - h_start;
            cursor = hb;
            for (;;) {
                cfw_http_header_t hdr;
                st = cfw_http_headers_next(&cursor, &hcursor, &hdr,
                                           &hdone);
                if (st != CFW_OK) {
                    return st;
                }
                if (hdone) {
                    break;
                }
                {
                    static const char ct[] = "content-type";
                    static const char cd[] =
                        "content-disposition";
                    cfw_strview_t ctv;
                    cfw_strview_t cdv;
                    int is_ct = 0;
                    int is_cd = 0;
                    cfw_strview_init(&ctv, ct, sizeof(ct) - 1);
                    cfw_strview_init(&cdv, cd, sizeof(cd) - 1);
                    if (cfw_strview_equal_nocase(&hdr.name, &ctv,
                                                 &is_ct) != CFW_OK) {
                        return CFW_ERROR_INTERNAL;
                    }
                    if (cfw_strview_equal_nocase(&hdr.name, &cdv,
                                                 &is_cd) != CFW_OK) {
                        return CFW_ERROR_INTERNAL;
                    }
                    if (is_ct) {
                        part.content_type = hdr.value;
                    } else if (is_cd) {
                        st = cfw_mp_disposition(&hdr.value, &part,
                                                &out);
                        if (st != CFW_OK) {
                            return st;
                        }
                    }
                }
            }
            part.header_block.data = (const char *)(b + h_start);
            part.header_block.size = h_end - h_start;
        }

        /* Part body runs to the next CRLF + "--" + boundary; matches
         * whose trailer is not a valid delimiter continuation are
         * skipped. */
        dpos = cfw_mp_find_delim(body, boundary, body_start, 1);
        trailer = -1;
        while (dpos != (size_t)-1) {
            trailer = cfw_mp_delim_trailer(body,
                                           dpos + boundary->size + 2,
                                           &next);
            if (trailer >= 0) {
                break;
            }
            dpos = cfw_mp_find_delim(body, boundary, dpos + 1, 1);
        }
        if (dpos == (size_t)-1) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }

        /* dpos is the index of the first '-' of "--boundary"; the
         * body ends at the CRLF two bytes before it. */
        if (cfw_span_init(&part.body, b + body_start,
                          (dpos - 2) - body_start) != CFW_OK) {
            return CFW_ERROR_INTERNAL;
        }

        if (stored < part_capacity) {
            parts[stored] = part;
            stored++;
        } else {
            dropped++;
        }

        if (trailer == 1) {
            break;
        }
        pos = next;
    }

    *count_out = stored;
    if (dropped_out != NULL) {
        *dropped_out = dropped;
    }
    return CFW_OK;
}
