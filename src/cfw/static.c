/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/static.h>

#include <cfw/fs.h>
#include <cfw/http.h>
#include <cfw/strview.h>

#include <string.h>

#define CFW_STATIC_PATH_CAP 4096u

/* Per-slot layout: cfw_file_stream_t followed by buffer_size scratch
 * bytes. The slot base doubles as the cfw_file_stream_t pointer (the
 * stream is the first member of the layout), so stream_user works for
 * both cfw_file_stream_read and cfw_file_stream_end. */

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

    stride = cfw_static_align_up(sizeof(cfw_file_stream_t) +
                                     options->buffer_size,
                                 2u * sizeof(void *));
    if (options->max_concurrent > (size_t)-1 / stride) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    total = stride * options->max_concurrent;

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
        cfw_file_stream_t *stream = (cfw_file_stream_t *)slot;
        cfw_file_init(&stream->file);
        stream->buffer = slot + sizeof(cfw_file_stream_t);
        stream->buffer_size = mount->buffer_size;
    }
    mount->ready = 1;
    return CFW_OK;
}

/* Returns a free slot (file handle invalid) or NULL. */
static unsigned char *cfw_static_acquire(cfw_static_mount_t *mount)
{
    size_t i;

    for (i = 0; i < mount->slot_count; i++) {
        unsigned char *slot = mount->slots + i * mount->slot_stride;
        cfw_file_stream_t *stream = (cfw_file_stream_t *)slot;
        if (!cfw_file_valid(&stream->file)) {
            return slot;
        }
    }
    return NULL;
}

static void cfw_static_respond(cfw_response_t *res,
                               cfw_http_status_t status)
{
    res->status = status;
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
    char fspath[CFW_STATIC_PATH_CAP];
    size_t dlen = 0;
    size_t rootlen;
    int is_dir = 0;
    uint64_t fsize = 0;
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
        if (flen + 11u >= sizeof(fspath)) {
            cfw_static_respond(res, CFW_HTTP_STATUS_URI_TOO_LONG);
            return CFW_OK;
        }
        memcpy(fspath + flen, "/index.html", 11u);
        fspath[flen + 11u] = '\0';
    }
    status = cfw_file_size(fspath, &fsize);
    if (status != CFW_OK) {
        /* Missing file, or a directory without index.html. */
        cfw_static_respond(res, is_dir ? CFW_HTTP_STATUS_FORBIDDEN
                                       : CFW_HTTP_STATUS_NOT_FOUND);
        (void)fsize;
        return CFW_OK;
    }

    if (cfw_static_set_type(res, cfw_static_content_type(fspath)) !=
        CFW_OK) {
        cfw_static_respond(res, CFW_HTTP_STATUS_INTERNAL_SERVER_ERROR);
        return CFW_OK;
    }

    if (is_head) {
        res->status = CFW_HTTP_STATUS_OK;
        return CFW_OK;
    }

    {
        unsigned char *slot = cfw_static_acquire(mount);
        cfw_file_stream_t *stream;
        if (slot == NULL) {
            cfw_static_respond(res, CFW_HTTP_STATUS_SERVICE_UNAVAILABLE);
            return CFW_OK;
        }
        stream = (cfw_file_stream_t *)slot;
        stream->buffer = slot + sizeof(cfw_file_stream_t);
        stream->buffer_size = mount->buffer_size;
        if (cfw_file_stream_init(stream, fspath, stream->buffer,
                                 mount->buffer_size) != CFW_OK) {
            cfw_static_respond(res, CFW_HTTP_STATUS_NOT_FOUND);
            return CFW_OK;
        }
        res->status = CFW_HTTP_STATUS_OK;
        res->stream = cfw_file_stream_read;
        res->stream_user = slot;
        res->stream_end = cfw_file_stream_end;
    }
    return CFW_OK;
}
