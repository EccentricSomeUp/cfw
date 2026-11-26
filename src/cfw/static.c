/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/static.h>

#include "private/fs_confined.h"

#include <cfw/fs.h>
#include <cfw/http.h>
#include <cfw/strview.h>

#include <string.h>

#define CFW_STATIC_PATH_CAP 4096u

/* Per-slot layout: cfw_static_stream_t followed by buffer_size
 * scratch bytes. The slot base doubles as the stream pointer (the
 * stream struct is the first member of the layout), so stream_user
 * works for both the read and end hooks. `remaining` bounds how many
 * bytes the stream emits, which is what makes HTTP Range responses
 * possible without a second stream type. */
typedef struct {
    cfw_file_t file;
    unsigned char *buffer;
    size_t buffer_size;
    uint64_t remaining;
    int eof;
} cfw_static_stream_t;

/* Response-header value scratch. Dynamic values (Last-Modified,
 * Content-Range) must stay valid until the transport serializes the
 * response head, which happens after cfw_static_serve returns — stack
 * storage would dangle, and the dispatch arena is
 * configuration-lifetime. A small ring carved from the same arena
 * allocation as the stream slots provides bounded, allocation-free
 * per-response storage. Values are only needed until the head is
 * serialized, which always precedes the next request on the
 * single-threaded server, so ring reuse is safe; a mount shared
 * across threads requires external synchronization, exactly like the
 * slot pool. */
#define CFW_STATIC_SCRATCH_SLOTS 8u
#define CFW_STATIC_SCRATCH_SLOT_SIZE 192u

typedef struct {
    char last_modified[30];
    char content_range[80];
    char spare[82]; /* reserved for future validators (ETag) */
} cfw_static_scratch_slot_t;

typedef struct {
    size_t next;
    cfw_static_scratch_slot_t slots[CFW_STATIC_SCRATCH_SLOTS];
} cfw_static_scratch_t;

static size_t cfw_static_align_up(size_t n, size_t a)
{
    return (n + a - 1u) / a * a;
}

static int cfw_static_hexval(char c, unsigned int *out)
{
    if (c >= '0' && c <= '9') {
        *out = (unsigned int)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (unsigned int)(c - 'a' + 10);
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (unsigned int)(c - 'A' + 10);
        return 1;
    }
    return 0;
}

/* Percent-decodes src[0..len) into dst (NUL-terminated, capacity cap).
 * Rejects malformed escapes, NUL bytes, and backslashes. Returns
 * CFW_OK or CFW_ERROR_INVALID_ARGUMENT. */
static cfw_status_t cfw_static_decode(const char *src, size_t len,
                                      char *dst, size_t cap,
                                      size_t *out_len)
{
    size_t i = 0;
    size_t o = 0;

    while (i < len) {
        unsigned int hi, lo;
        char c = src[i];
        if (c == '\\') {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (c == '%') {
            if (i + 2 >= len ||
                !cfw_static_hexval(src[i + 1], &hi) ||
                !cfw_static_hexval(src[i + 2], &lo)) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            c = (char)((hi << 4) | lo);
            /* Reject NUL and backslash even when percent-encoded:
             * NUL would truncate the fs path, and '\\' is a separator
             * on Windows. */
            if (c == '\0' || c == '\\') {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            i += 3;
        } else {
            i += 1;
        }
        if (o + 1 >= cap) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    *out_len = o;
    return CFW_OK;
}

/* Rejects any "/"-separated segment equal to ".." in the decoded
 * path. "." segments and empty segments are tolerated (the filesystem
 * resolves them harmlessly). */
static int cfw_static_has_dotdot(const char *path)
{
    const char *p = path;

    for (;;) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash != NULL ? (size_t)(slash - p)
                                      : strlen(p);
        if (seglen == 2 && p[0] == '.' && p[1] == '.') {
            return 1;
        }
        if (slash == NULL) {
            return 0;
        }
        p = slash + 1;
    }
}

typedef struct {
    const char *ext;
    const char *type;
} cfw_static_mime_t;

static const cfw_static_mime_t cfw_static_mime[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"xml", "application/xml"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"ico", "image/x-icon"},
    {"bmp", "image/bmp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mp3", "audio/mpeg"},
    {"wav", "audio/wav"},
    {"wasm", "application/wasm"},
    {"zip", "application/zip"},
};

static int cfw_static_ieq(char a, char b)
{
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
    }
    return a == b;
}

static const char *cfw_static_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    size_t i;

    if (dot == NULL || (slash != NULL && dot < slash)) {
        return "application/octet-stream";
    }
    dot++;
    for (i = 0; i < sizeof(cfw_static_mime) / sizeof(cfw_static_mime[0]);
         i++) {
        const char *e = cfw_static_mime[i].ext;
        const char *p = dot;
        while (*e != '\0' && *p != '\0' && cfw_static_ieq(*p, *e)) {
            p++;
            e++;
        }
        if (*e == '\0' && *p == '\0') {
            return cfw_static_mime[i].type;
        }
    }
    return "application/octet-stream";
}

static cfw_status_t cfw_static_set_type(cfw_response_t *res,
                                        const char *type)
{
    cfw_strview_t name;
    cfw_strview_t value;

    cfw_strview_from_cstr(&name, "Content-Type");
    cfw_strview_from_cstr(&value, type);
    return cfw_response_header_add(res, &name, &value);
}

/* The scratch ring lives in the same arena allocation, directly after
 * the slot array (slot_stride is a multiple of the pointer alignment,
 * so the base stays suitably aligned). */
static cfw_static_scratch_t *cfw_static_scratch(cfw_static_mount_t *mount)
{
    return (cfw_static_scratch_t *)(mount->slots +
                                    mount->slot_count * mount->slot_stride);
}

static cfw_static_scratch_slot_t *cfw_static_scratch_acquire(
    cfw_static_mount_t *mount)
{
    cfw_static_scratch_t *scratch = cfw_static_scratch(mount);
    cfw_static_scratch_slot_t *slot = &scratch->slots[scratch->next];
    scratch->next = (scratch->next + 1u) % CFW_STATIC_SCRATCH_SLOTS;
    return slot;
}

cfw_status_t cfw_static_mount_init(cfw_static_mount_t *mount,
                                   const cfw_static_options_t *options)
{
    size_t plen;
    size_t total;
    size_t stride;
    size_t i;

    if (mount == NULL || options == NULL || options->arena == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (options->root == NULL || options->root[0] == '\0' ||
        options->url_prefix == NULL || options->url_prefix[0] != '/' ||
        options->max_concurrent == 0 || options->buffer_size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    plen = strlen(options->url_prefix);
    if (plen > 1 && options->url_prefix[plen - 1] == '/') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    stride = cfw_static_align_up(sizeof(cfw_static_stream_t) +
                                     options->buffer_size,
                                 2u * sizeof(void *));
    if (options->max_concurrent > (size_t)-1 / stride) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if ((size_t)-1 - stride * options->max_concurrent <
        sizeof(cfw_static_scratch_t)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    total = stride * options->max_concurrent +
           sizeof(cfw_static_scratch_t);

    memset(mount, 0, sizeof(*mount));
    if (cfw_arena_alloc_zeroed(options->arena, total,
                               (void **)&mount->slots) != CFW_OK) {
        return CFW_ERROR_UNAVAILABLE;
    }

    mount->root = options->root;
    mount->url_prefix = options->url_prefix;
    mount->arena = options->arena;
    mount->slot_count = options->max_concurrent;
    mount->slot_stride = stride;
    mount->buffer_size = options->buffer_size;

    for (i = 0; i < mount->slot_count; i++) {
        unsigned char *slot = mount->slots + i * stride;
        cfw_static_stream_t *stream = (cfw_static_stream_t *)slot;
        cfw_file_init(&stream->file);
        stream->buffer = slot + sizeof(cfw_static_stream_t);
        stream->buffer_size = mount->buffer_size;
        stream->remaining = 0;
        stream->eof = 0;
    }
    memset(cfw_static_scratch(mount), 0, sizeof(cfw_static_scratch_t));
    mount->ready = 1;
    return CFW_OK;
}

/* Returns a free slot (file handle invalid) or NULL. */
static unsigned char *cfw_static_acquire(cfw_static_mount_t *mount)
{
    size_t i;

    for (i = 0; i < mount->slot_count; i++) {
        unsigned char *slot = mount->slots + i * mount->slot_stride;
        cfw_static_stream_t *stream = (cfw_static_stream_t *)slot;
        if (!cfw_file_valid(&stream->file)) {
            return slot;
        }
    }
    return NULL;
}

static cfw_status_t cfw_static_stream_read(void *user, cfw_span_t *chunk,
                                           int *eof)
{
    cfw_static_stream_t *stream = user;
    size_t want;
    size_t got = 0;
    cfw_status_t status;

    if (stream == NULL || chunk == NULL || eof == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *eof = 0;
    cfw_span_init(chunk, NULL, 0);

    if (stream->eof || stream->remaining == 0) {
        stream->eof = 1;
        *eof = 1;
        return CFW_OK;
    }
    want = stream->buffer_size;
    if (stream->remaining < (uint64_t)want) {
        want = (size_t)stream->remaining;
    }
    status = cfw_file_read(&stream->file, stream->buffer, want, &got);
    if (status != CFW_OK) {
        return status;
    }
    if (got == 0) {
        stream->eof = 1;
        *eof = 1;
        return CFW_OK;
    }
    stream->remaining -= (uint64_t)got;
    return cfw_span_init(chunk, stream->buffer, got);
}

static void cfw_static_stream_end(void *user)
{
    cfw_static_stream_t *stream = user;
    if (stream != NULL) {
        cfw_file_close(&stream->file);
    }
}

static void cfw_static_respond(cfw_response_t *res,
                               cfw_http_status_t status)
{
    res->status = status;
}

typedef enum {
    CFW_STATIC_PLAN_FULL = 0,
    CFW_STATIC_PLAN_RANGE,
    CFW_STATIC_PLAN_NOT_MODIFIED,
    CFW_STATIC_PLAN_PRECONDITION_FAILED,
    CFW_STATIC_PLAN_RANGE_UNSATISFIABLE
} cfw_static_plan_action_t;

typedef struct {
    cfw_static_plan_action_t action;
    uint64_t start;
    uint64_t end; /* inclusive */
} cfw_static_plan_t;

/* Parses an HTTP-date request header; returns 1 and *out on success.
 * Absent or malformed values are ignored (return 0), matching RFC 9110
 * guidance that invalid field values do not make a request
 * unprocessable here. */
static int cfw_static_header_date(cfw_request_t *req, const char *name,
                                  int64_t *out)
{
    cfw_strview_t hn;
    cfw_strview_t value;

    cfw_strview_from_cstr(&hn, name);
    if (cfw_request_header(req, &hn, &value) != CFW_OK) {
        return 0;
    }
    return cfw_http_date_parse(&value, out) == CFW_OK;
}

static int cfw_static_parse_u64(const unsigned char **p,
                                const unsigned char *end, uint64_t *out)
{
    uint64_t v = 0;
    int digits = 0;

    while (*p < end && **p >= '0' && **p <= '9') {
        uint64_t d = (uint64_t)(**p - '0');
        if (v > (UINT64_MAX - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
        (*p)++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

static int cfw_static_is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* Parses a Range header value for a single byte range. Returns 1 with
 * the start/end out-params set (inclusive, clamped to the
 * representation) when the range is satisfiable, 0 when the header
 * must be ignored (unknown
 * unit, malformed syntax, multiple ranges, or an invalid
 * last-byte-pos < first-byte-pos spec), and -1 when the range is
 * well-formed but unsatisfiable for `size`. */
static int cfw_static_parse_range(const cfw_strview_t *value,
                                  uint64_t size, uint64_t *start,
                                  uint64_t *end)
{
    static const char unit[] = "bytes=";
    const unsigned char *p;
    const unsigned char *limit;
    uint64_t first;
    uint64_t last = UINT64_MAX;
    int suffix_form = 0;
    size_t i;

    if (value->data == NULL || value->size < sizeof(unit) - 1u) {
        return 0;
    }
    p = (const unsigned char *)value->data;
    limit = p + value->size;
    for (i = 0; i < sizeof(unit) - 1u; i++) {
        unsigned char c = p[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        if (c != (unsigned char)unit[i]) {
            return 0;
        }
    }
    p += sizeof(unit) - 1u;

    if (p < limit && *p == '-') {
        suffix_form = 1;
        p++;
        if (!cfw_static_parse_u64(&p, limit, &first)) {
            return 0;
        }
    } else {
        if (!cfw_static_parse_u64(&p, limit, &first)) {
            return 0;
        }
        if (p >= limit || *p != '-') {
            return 0;
        }
        p++;
        if (p < limit && cfw_static_is_digit(*p) &&
            !cfw_static_parse_u64(&p, limit, &last)) {
            return 0;
        }
    }
    /* Only trailing HTTP whitespace may follow the range; anything
     * else (commas, extra specs, junk) makes the whole header
     * ignorable. */
    while (p < limit && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p != limit) {
        return 0;
    }

    if (suffix_form) {
        if (first == 0 || size == 0) {
            return -1;
        }
        if (first >= size) {
            *start = 0;
            *end = size - 1u;
        } else {
            *start = size - first;
            *end = size - 1u;
        }
        return 1;
    }
    if (first > last || size == 0 || first >= size) {
        return first > last ? 0 : -1;
    }
    *start = first;
    *end = last >= size - 1u ? size - 1u : last;
    return 1;
}

static size_t cfw_static_u64_dec(uint64_t value, char *out)
{
    char digits[20];
    size_t count = 0;
    size_t i = 0;

    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count != 0) {
        out[i++] = digits[--count];
    }
    out[i] = '\0';
    return i;
}

/* Writes the Content-Range value "bytes <start>-<end>/<size>", or the
 * complete-length form "bytes star/<size>" when complete is 0, into
 * out, which must hold at least 69 bytes. */
static void cfw_static_content_range(char *out, int complete,
                                     uint64_t start, uint64_t end,
                                     uint64_t size)
{
    static const char prefix[] = "bytes ";
    char *w = out;
    size_t i;

    for (i = 0; i < sizeof(prefix) - 1u; i++) {
        *w++ = prefix[i];
    }
    if (complete) {
        w += cfw_static_u64_dec(start, w);
        *w++ = '-';
        w += cfw_static_u64_dec(end, w);
    } else {
        *w++ = '*';
    }
    *w++ = '/';
    w += cfw_static_u64_dec(size, w);
    *w = '\0';
}

static cfw_status_t cfw_static_header(cfw_response_t *res,
                                      const char *name,
                                      const char *value, size_t vlen)
{
    cfw_strview_t hn;
    cfw_strview_t hv;

    cfw_strview_from_cstr(&hn, name);
    if (cfw_strview_init(&hv, value, vlen) != CFW_OK) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return cfw_response_header_add(res, &hn, &hv);
}

/* ------------------------------------------------------------------ */
/* Entity-tag validators (ADR 0040)                                    */
/* ------------------------------------------------------------------ */

/* The mount emits a strong entity-tag "<mtime>-<size>" — a heuristic
 * validator in the same family as common static servers: it changes
 * whenever the modification time or size does. */

static size_t cfw_static_i64_dec(int64_t value, char *out)
{
    uint64_t u;
    size_t n;

    if (value < 0) {
        *out++ = '-';
        u = ~(uint64_t)value + 1u;
    } else {
        u = (uint64_t)value;
    }
    n = cfw_static_u64_dec(u, out);
    return n + (value < 0 ? 1u : 0u);
}

/* Writes "<mtime>-<size>" (with surrounding quotes) into out, which
 * must hold at least 45 bytes. */
static void cfw_static_format_etag(char *out, int64_t mtime,
                                   uint64_t size)
{
    char *w = out;

    *w++ = '"';
    w += cfw_static_i64_dec(mtime, w);
    *w++ = '-';
    w += cfw_static_u64_dec(size, w);
    *w++ = '"';
    *w = '\0';
}

static void cfw_static_trim(cfw_strview_t *view)
{
    while (view->size != 0 &&
           (view->data[0] == ' ' || view->data[0] == '\t')) {
        view->data++;
        view->size--;
    }
    while (view->size != 0 &&
           (view->data[view->size - 1] == ' ' ||
            view->data[view->size - 1] == '\t')) {
        view->size--;
    }
}

/* Extracts the opaque-tag (the quoted string, quotes included) from
 * one entity-tag list member, skipping an optional "W/" prefix.
 * Returns 1 on success; *weak reports the prefix. Members that are
 * not well-formed entity-tags return 0. */
static int cfw_static_etag_opaque(const cfw_strview_t *item,
                                  cfw_strview_t *opaque, int *weak)
{
    size_t i = 0;
    size_t j;

    *weak = 0;
    if (item->size >= 2 && item->data[0] == 'W' && item->data[1] == '/') {
        *weak = 1;
        i = 2;
    }
    if (item->size - i < 2 || item->data[i] != '"') {
        return 0;
    }
    j = i + 1;
    while (j < item->size && item->data[j] != '"') {
        unsigned char c = (unsigned char)item->data[j];
        /* etagc: %x21 / %x23-7E / obs-text — no CTLs, no space. */
        if (c < 0x21 || c == 0x7f) {
            return 0;
        }
        j++;
    }
    if (j >= item->size || j + 1 != item->size) {
        return 0;
    }
    return cfw_strview_init(opaque, item->data + i,
                            j - i + 1) == CFW_OK;
}

/* Evaluates an If-Match/If-None-Match style header list against the
 * mount's etag (the full quoted opaque-tag). weak=1 applies weak
 * comparison (W/ prefixes ignored on both sides — our etag has none);
 * weak=0 requires strong comparison (W/ members never match).
 * "*" matches any current representation. Returns 1 on match, 0 when
 * the header is a valid list with no match, and -1 when the header is
 * absent or contains no usable member (treated as ignorable). */
static int cfw_static_etag_list_match(const cfw_request_t *req,
                                      const char *name,
                                      const cfw_strview_t *etag,
                                      int weak)
{
    cfw_strview_t hn;
    cfw_strview_t value;
    size_t pos = 0;
    int saw_item = 0;

    cfw_strview_from_cstr(&hn, name);
    if (cfw_request_header(req, &hn, &value) != CFW_OK ||
        value.size == 0 || value.data == NULL) {
        return -1;
    }
    for (;;) {
        size_t start = pos;
        size_t end;
        cfw_strview_t item;
        cfw_strview_t opaque;
        int item_weak;
        int last;

        while (pos < value.size && value.data[pos] != ',') {
            pos++;
        }
        end = pos;
        last = pos >= value.size;
        if (!last) {
            pos++;
        }
        cfw_strview_init(&item, value.data + start, end - start);
        cfw_static_trim(&item);
        if (item.size == 1 && item.data[0] == '*') {
            return 1;
        }
        if (cfw_static_etag_opaque(&item, &opaque, &item_weak)) {
            saw_item = 1;
            if (opaque.size == etag->size &&
                memcmp(opaque.data, etag->data, opaque.size) == 0 &&
                (weak || !item_weak)) {
                return 1;
            }
        }
        if (last) {
            break;
        }
    }
    return saw_item ? 0 : -1;
}

/* Evaluates the conditional headers in RFC 9110 section 13.2.2 order
 * (If-Match, If-Unmodified-Since, If-None-Match, If-Modified-Since)
 * restricted to the validators this mount emits, then the Range
 * header gated by If-Range. `etag` is the mount's current entity-tag
 * (opaque-tag form, quotes included). */
static void cfw_static_plan(cfw_request_t *req, int64_t mtime,
                            uint64_t size,
                            const cfw_strview_t *etag,
                            cfw_static_plan_t *plan)
{
    cfw_strview_t hn;
    cfw_strview_t range_value;
    cfw_strview_t if_range_value;
    int64_t date;
    int pr;
    int im;
    int inm;

    plan->action = CFW_STATIC_PLAN_FULL;
    plan->start = 0;
    plan->end = 0;

    /* If-Match (strong comparison): a valid non-match rejects with
     * 412; a match satisfies the precondition and skips
     * If-Unmodified-Since. */
    im = cfw_static_etag_list_match(req, "If-Match", etag, 0);
    if (im == 0) {
        plan->action = CFW_STATIC_PLAN_PRECONDITION_FAILED;
        return;
    }
    if (im != 1 &&
        cfw_static_header_date(req, "If-Unmodified-Since", &date) &&
        mtime > date) {
        plan->action = CFW_STATIC_PLAN_PRECONDITION_FAILED;
        return;
    }

    /* If-None-Match (weak comparison for GET/HEAD): a match yields
     * 304; a valid list without a match suppresses
     * If-Modified-Since. */
    inm = cfw_static_etag_list_match(req, "If-None-Match", etag, 1);
    if (inm == 1) {
        plan->action = CFW_STATIC_PLAN_NOT_MODIFIED;
        return;
    }
    if (inm != 0 &&
        cfw_static_header_date(req, "If-Modified-Since", &date) &&
        mtime <= date) {
        plan->action = CFW_STATIC_PLAN_NOT_MODIFIED;
        return;
    }

    cfw_strview_from_cstr(&hn, "Range");
    if (cfw_request_header(req, &hn, &range_value) != CFW_OK) {
        return;
    }
    cfw_strview_from_cstr(&hn, "If-Range");
    if (cfw_request_header(req, &hn, &if_range_value) == CFW_OK) {
        /* If-Range carries either an HTTP-date or a single
         * entity-tag. A date honors the range only when the file is
         * unchanged since then; an entity-tag must match strongly
         * (weak tags never do). Anything else yields the full
         * representation. */
        if (cfw_http_date_parse(&if_range_value, &date) == CFW_OK) {
            if (mtime > date) {
                return;
            }
        } else {
            cfw_strview_t tag = if_range_value;
            cfw_static_trim(&tag);
            if (tag.size != etag->size ||
                memcmp(tag.data, etag->data, tag.size) != 0) {
                return;
            }
        }
    }
    pr = cfw_static_parse_range(&range_value, size, &plan->start,
                                &plan->end);
    if (pr < 0) {
        plan->action = CFW_STATIC_PLAN_RANGE_UNSATISFIABLE;
    } else if (pr > 0) {
        plan->action = CFW_STATIC_PLAN_RANGE;
    }
}

cfw_status_t cfw_static_serve(cfw_request_t *req, cfw_response_t *res,
                              cfw_dispatch_next_t next,
                              cfw_dispatch_frame_t *frame, void *user)
{
    cfw_static_mount_t *mount = user;
    const char *prefix;
    const char *after;
    size_t plen;
    size_t alen;
    char decoded[CFW_STATIC_PATH_CAP];
    char rel[CFW_STATIC_PATH_CAP];
    char fspath[CFW_STATIC_PATH_CAP];
    size_t dlen = 0;
    size_t rootlen;
    int is_dir = 0;
    cfw_status_t status;
    int is_head;

    if (req == NULL || res == NULL || next == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (mount == NULL || !mount->ready) {
        return next(frame);
    }
    is_head = req->method == CFW_HTTP_METHOD_HEAD;
    if (req->method != CFW_HTTP_METHOD_GET && !is_head) {
        return next(frame);
    }

    /* Prefix match on the raw (still encoded) path. */
    prefix = mount->url_prefix;
    plen = strlen(prefix);
    if (req->path.size < plen ||
        memcmp(req->path.data, prefix, plen) != 0) {
        return next(frame);
    }
    after = req->path.data + plen;
    alen = req->path.size - plen;
    if (alen > 0) {
        int prefix_slash = prefix[plen - 1] == '/';
        if (!prefix_slash && after[0] != '/') {
            return next(frame); /* e.g. /staticx vs /static */
        }
        if (after[0] == '/') {
            after++;
            alen--;
        }
    }

    /* Decode + validate the suffix. */
    if (alen > 0 &&
        cfw_static_decode(after, alen, decoded, sizeof(decoded),
                          &dlen) != CFW_OK) {
        cfw_static_respond(res, CFW_HTTP_STATUS_BAD_REQUEST);
        return CFW_OK;
    }
    decoded[dlen] = '\0';
    memcpy(rel, decoded, dlen + 1u);
    if (cfw_static_has_dotdot(decoded)) {
        cfw_static_respond(res, CFW_HTTP_STATUS_NOT_FOUND);
        return CFW_OK;
    }

    /* Join root + suffix. */
    rootlen = strlen(mount->root);
    {
        int root_slash = mount->root[rootlen - 1] == '/';
        size_t need = rootlen + (root_slash ? 0u : 1u) + dlen + 1u;
        if (need > sizeof(fspath)) {
            cfw_static_respond(res, CFW_HTTP_STATUS_URI_TOO_LONG);
            return CFW_OK;
        }
        memcpy(fspath, mount->root, rootlen);
        if (!root_slash) {
            fspath[rootlen] = '/';
            rootlen++;
        }
        memcpy(fspath + rootlen, decoded, dlen + 1u);
    }

    /* Resolve directories to index.html. */
    status = cfw_file_is_directory(fspath, &is_dir);
    if (status == CFW_ERROR_IO) {
        cfw_static_respond(res, CFW_HTTP_STATUS_NOT_FOUND);
        return CFW_OK;
    }
    if (status != CFW_OK) {
        cfw_static_respond(res, CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR);
        return CFW_OK;
    }
    if (is_dir) {
        size_t flen = strlen(fspath);
        size_t rlen = strlen(rel);
        int rel_slash = rlen > 0 && rel[rlen - 1] != '/';
        if (flen + 11u >= sizeof(fspath) ||
            rlen + (size_t)rel_slash + 11u > sizeof(rel)) {
            cfw_static_respond(res, CFW_HTTP_STATUS_URI_TOO_LONG);
            return CFW_OK;
        }
        memcpy(fspath + flen, "/index.html", 11u);
        fspath[flen + 11u] = '\0';
        if (rel_slash) {
            rel[rlen++] = '/';
        }
        memcpy(rel + rlen, "index.html", 11u);
    }
    /* Open the confined file first so size and mtime come from fstat
     * on the descriptor the stream will actually read — the same
     * object, with no stat-then-open gap (ADR 0039). */
    {
        cfw_file_t file;
        cfw_file_stat_t fstat;
        cfw_static_plan_t plan;
        cfw_static_scratch_slot_t *scratch;
        cfw_http_status_t http_status;
        cfw_strview_t etag;
        int serve_body;

        cfw_file_init(&file);
        status = cfw_file_open_confined(&file, mount->root, rel);
        if (status != CFW_OK) {
            /* Missing file, or a directory without index.html. */
            cfw_static_respond(res, is_dir ? CFW_HTTP_STATUS_FORBIDDEN
                                           : CFW_HTTP_STATUS_NOT_FOUND);
            return CFW_OK;
        }
        status = cfw_file_fstat(&file, &fstat);
        if (status != CFW_OK) {
            cfw_file_close(&file);
            cfw_static_respond(res, CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR);
            return CFW_OK;
        }

        scratch = cfw_static_scratch_acquire(mount);
        cfw_static_format_etag(scratch->spare, fstat.mtime_seconds,
                              fstat.size);
        cfw_strview_init(&etag, scratch->spare,
                         strlen(scratch->spare));
        cfw_static_plan(req, fstat.mtime_seconds, fstat.size, &etag,
                        &plan);
        switch (plan.action) {
        case CFW_STATIC_PLAN_NOT_MODIFIED:
            http_status = CFW_HTTP_STATUS_NOT_MODIFIED;
            break;
        case CFW_STATIC_PLAN_PRECONDITION_FAILED:
            http_status = CFW_HTTP_STATUS_PRECONDITION_FAILED;
            break;
        case CFW_STATIC_PLAN_RANGE_UNSATISFIABLE:
            http_status = CFW_HTTP_STATUS_RANGE_NOT_SATISFIABLE;
            break;
        case CFW_STATIC_PLAN_RANGE:
            http_status = CFW_HTTP_STATUS_PARTIAL_CONTENT;
            break;
        default:
            http_status = CFW_HTTP_STATUS_OK;
            break;
        }
        serve_body = !is_head &&
                     (plan.action == CFW_STATIC_PLAN_FULL ||
                      plan.action == CFW_STATIC_PLAN_RANGE);

        /* Pre-flight the slot pool so a 503 does not carry file
         * headers; acquire() does not reserve, so the real acquire
         * happens again after the headers are written. */
        if (serve_body && cfw_static_acquire(mount) == NULL) {
            cfw_file_close(&file);
            cfw_static_respond(res, CFW_HTTP_STATUS_SERVICE_UNAVAILABLE);
            return CFW_OK;
        }

        if ((plan.action == CFW_STATIC_PLAN_FULL ||
             plan.action == CFW_STATIC_PLAN_RANGE ||
             plan.action == CFW_STATIC_PLAN_NOT_MODIFIED) &&
            cfw_static_set_type(res,
                                cfw_static_content_type(fspath)) !=
                CFW_OK) {
            goto header_error;
        }
        if (plan.action == CFW_STATIC_PLAN_FULL ||
            plan.action == CFW_STATIC_PLAN_RANGE ||
            plan.action == CFW_STATIC_PLAN_NOT_MODIFIED ||
            plan.action == CFW_STATIC_PLAN_RANGE_UNSATISFIABLE) {
            if (cfw_http_date_format(fstat.mtime_seconds,
                                     scratch->last_modified,
                                     sizeof(scratch->last_modified)) ==
                    CFW_OK &&
                cfw_static_header(res, "Last-Modified",
                                  scratch->last_modified, 29) != CFW_OK) {
                goto header_error;
            }
            if (cfw_static_header(res, "ETag", scratch->spare,
                                  strlen(scratch->spare)) != CFW_OK) {
                goto header_error;
            }
            if (cfw_static_header(res, "Accept-Ranges", "bytes", 5) !=
                CFW_OK) {
                goto header_error;
            }
        }
        if (plan.action == CFW_STATIC_PLAN_RANGE) {
            cfw_static_content_range(scratch->content_range, 1,
                                     plan.start, plan.end, fstat.size);
            if (cfw_static_header(res, "Content-Range",
                                  scratch->content_range,
                                  strlen(scratch->content_range)) !=
                CFW_OK) {
                goto header_error;
            }
        } else if (plan.action == CFW_STATIC_PLAN_RANGE_UNSATISFIABLE) {
            cfw_static_content_range(scratch->content_range, 0, 0, 0,
                                     fstat.size);
            if (cfw_static_header(res, "Content-Range",
                                  scratch->content_range,
                                  strlen(scratch->content_range)) !=
                CFW_OK) {
                goto header_error;
            }
        }

        /* Declare the body length for known-size plans so the server
         * emits Content-Length + identity framing instead of chunked
         * (GET) and so HEAD reports the real length. */
        if (plan.action == CFW_STATIC_PLAN_FULL) {
            res->stream_size = fstat.size;
        } else if (plan.action == CFW_STATIC_PLAN_RANGE) {
            res->stream_size = (uint64_t)(plan.end - plan.start + 1u);
        }

        if (!serve_body) {
            cfw_file_close(&file);
            res->status = http_status;
            return CFW_OK;
        }

        {
            unsigned char *slot = cfw_static_acquire(mount);
            cfw_static_stream_t *stream;

            if (slot == NULL) {
                cfw_file_close(&file);
                cfw_static_respond(res, CFW_HTTP_STATUS_SERVICE_UNAVAILABLE);
                return CFW_OK;
            }
            stream = (cfw_static_stream_t *)slot;
            stream->buffer = slot + sizeof(cfw_static_stream_t);
            stream->buffer_size = mount->buffer_size;
            stream->remaining = plan.action == CFW_STATIC_PLAN_RANGE
                                    ? plan.end - plan.start + 1u
                                    : fstat.size;
            stream->eof = 0;
            cfw_file_init(&stream->file);
            if (plan.action == CFW_STATIC_PLAN_RANGE &&
                cfw_file_seek(&file, plan.start) != CFW_OK) {
                cfw_static_respond(res, CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR);
                goto seek_error;
            }
            /* Transfer handle ownership into the slot. */
            stream->file = file;
            cfw_file_init(&file);

            res->status = http_status;
            res->stream = cfw_static_stream_read;
            res->stream_user = slot;
            res->stream_end = cfw_static_stream_end;
            return CFW_OK;

        seek_error:
            cfw_file_close(&file);
            return CFW_OK;
        }

    header_error:
        cfw_file_close(&file);
        cfw_static_respond(res, CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR);
        return CFW_OK;
    }
}
