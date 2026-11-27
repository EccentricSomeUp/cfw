/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/http_client.h>

#include <cfw/pollset.h>

#include <cfw/net.h>
#include <cfw/span.h>
#include <cfw/version.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CFW_PLATFORM_WINDOWS
    #include <winsock2.h>
#else
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

/* ------------------------------------------------------------------ */
/* Portable response accessors                                         */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_http_client_response_head(
    const cfw_http_client_response_t *response,
    cfw_http_response_head_t *head)
{
    cfw_strview_t head_view;

    if (response == NULL || head == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (response->head.size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    head_view.data = (const char *)response->head.data;
    head_view.size = response->head.size;
    return cfw_http_response_head_parse(&head_view, head);
}

void cfw_http_client_response_free(cfw_http_client_response_t *response)
{
    if (response == NULL) {
        return;
    }
    cfw_buffer_free(&response->head);
    cfw_buffer_free(&response->body);
    memset(response, 0, sizeof(*response));
}

#define CFW_HTTP_CLIENT_READ_SIZE 16384u
#define CFW_HTTP_CLIENT_DEFAULT_TIMEOUT_MS 30000ll
#define CFW_HTTP_CLIENT_DEFAULT_MAX_HEAD (64u * 1024u)
#define CFW_HTTP_CLIENT_DEFAULT_MAX_BODY (16ull * 1024ull * 1024ull)
#define CFW_HTTP_CLIENT_MAX_REDIRECTS 16u
#define CFW_HTTP_CLIENT_MAX_RETRIES 4u

/* Framework-managed headers the caller may not override: duplicates
 * would corrupt framing or routing. Compared case-insensitively. */
static const char *const cfw_client_forbidden_headers[] = {
    "host",
    "user-agent",
    "content-length",
    "connection",
    "keep-alive",
    "proxy-connection",
    "transfer-encoding",
    "upgrade",
    "te",
    "trailer",
    "expect",
};

/* ------------------------------------------------------------------ */
/* Connection state                                                    */
/* ------------------------------------------------------------------ */

/* Persistent connection plus the per-session bookkeeping: the
 * authority the open connection is bound to, leftover bytes past the
 * last response, and the serialized TLS session offered to the next
 * connection (ADR 0062/0078). One-shot requests use a stack-allocated
 * state that is freed after a single exchange. */
typedef struct {
    cfw_pollset_t *pollset;
    cfw_fd_t fd;
    cfw_tls_conn_t *tls;
    int conn_open;
    /* Leftover bytes read past the previous response head/body and
     * the TLS want-direction is not tracked here (per-call). */
    cfw_buffer_t pending;
    size_t pending_pos;
    int input_closed;
    /* Authority binding of the open connection. */
    cfw_buffer_t auth_host; /* content bytes only; compared case-insensitively */
    uint16_t auth_port;
    int auth_tls;
    /* Serialized TLS session for resumption across reconnects. */
    cfw_buffer_t tls_session;
} cfw_hc_state_t;

/* Per-call deadline shared by every hop and retry of one perform. */
typedef struct {
    cfw_timepoint_t deadline;
    int has_deadline;
} cfw_hc_clock_t;

/* Effective request for one exchange hop: owned host/target buffers so
 * redirects can rewrite them. headers/headers_owned/body are borrowed
 * or owned respectively. */
typedef struct {
    cfw_buffer_t host; /* NUL-terminated */
    uint16_t port;
    cfw_http_method_t method;
    cfw_buffer_t target;
    const cfw_http_header_t *headers;
    size_t header_count;
    cfw_http_header_t *headers_owned; /* filtered copy, or NULL */
    const void *body;
    size_t body_size;
    int idempotent;
} cfw_hc_effective_t;

/* Response-body delivery for one exchange: append to `buffer`, stream
 * to `sink`, or discard (redirect hops). max_body bounds the
 * cumulative total in every mode. */
typedef struct {
    cfw_buffer_t *buffer;
    cfw_status_t (*sink)(void *user, const void *data, size_t size);
    void *sink_user;
    uint64_t max_body;
    uint64_t total;
    int sink_started;
} cfw_hc_body_t;

static int cfw_hc_method_idempotent(cfw_http_method_t method)
{
    switch (method) {
    case CFW_HTTP_METHOD_GET:
    case CFW_HTTP_METHOD_HEAD:
    case CFW_HTTP_METHOD_PUT:
    case CFW_HTTP_METHOD_DELETE:
    case CFW_HTTP_METHOD_OPTIONS:
    case CFW_HTTP_METHOD_TRACE:
        return 1;
    default:
        return 0;
    }
}

static int cfw_client_name_is_forbidden(const cfw_strview_t *name)
{
    size_t i;
    for (i = 0; i < sizeof(cfw_client_forbidden_headers) /
                        sizeof(cfw_client_forbidden_headers[0]);
         i++) {
        cfw_strview_t forbidden;
        int equal = 0;
        if (cfw_strview_from_cstr(&forbidden,
                                  cfw_client_forbidden_headers[i]) !=
                CFW_OK ||
            cfw_strview_equal_nocase(name, &forbidden, &equal) != CFW_OK) {
            return 0;
        }
        if (equal) {
            return 1;
        }
    }
    return 0;
}

/* Replaces buffer content with `size` bytes plus a NUL at data[size]
 * (within reserved capacity). size excludes the terminator, so
 * string consumers see a proper cstr while size-based consumers see
 * content only. */
static cfw_status_t cfw_hc_buf_set(cfw_buffer_t *buf, const void *data,
                                   size_t size)
{
    cfw_buffer_clear(buf);
    if (cfw_buffer_reserve(buf, size + 1) != CFW_OK) {
        return CFW_ERROR_NOMEM;
    }
    if (size != 0) {
        memcpy(buf->data, data, size);
    }
    buf->data[size] = '\0';
    buf->size = size;
    return CFW_OK;
}

/* Finds the first header named `name` (case-insensitive) in a parsed
 * response header block; returns 1 and stores its value in `out`,
 * or 0 when absent. */
static int cfw_hc_find_header(const cfw_strview_t *block,
                              const char *name, cfw_strview_t *out)
{
    cfw_strview_t want;
    cfw_strview_t block_copy = *block;
    size_t cursor = 0;
    int done = 0;

    if (cfw_strview_from_cstr(&want, name) != CFW_OK) {
        return 0;
    }
    for (;;) {
        cfw_http_header_t header;
        cfw_status_t status =
            cfw_http_headers_next(&block_copy, &cursor, &header, &done);
        int equal = 0;
        if (status != CFW_OK || done) {
            return 0;
        }
        if (cfw_strview_equal_nocase(&header.name, &want, &equal) ==
                CFW_OK &&
            equal) {
            *out = header.value;
            return 1;
        }
    }
}

/* Nonzero when any `name` header value carries `token` as a
 * comma-separated token (case-insensitive), e.g. Connection. */
static int cfw_hc_header_has_token(const cfw_strview_t *block,
                                   const char *name, const char *token)
{
    cfw_strview_t want_name;
    cfw_strview_t want_token;
    cfw_strview_t block_copy = *block;
    size_t cursor = 0;
    int done = 0;

    if (cfw_strview_from_cstr(&want_name, name) != CFW_OK ||
        cfw_strview_from_cstr(&want_token, token) != CFW_OK) {
        return 0;
    }
    for (;;) {
        cfw_http_header_t header;
        cfw_status_t status =
            cfw_http_headers_next(&block_copy, &cursor, &header, &done);
        int equal = 0;
        if (status != CFW_OK || done) {
            return 0;
        }
        if (cfw_strview_equal_nocase(&header.name, &want_name, &equal) ==
                CFW_OK &&
            equal) {
            /* Scan comma-separated tokens with OWS trimming. */
            size_t i = 0;
            while (i <= header.value.size) {
                size_t start = i;
                size_t end;
                cfw_strview_t piece;
                int peq = 0;
                while (i < header.value.size &&
                       header.value.data[i] != ',') {
                    i++;
                }
                end = i;
                if (i < header.value.size) {
                    i++; /* skip ',' */
                }
                while (start < end &&
                       (header.value.data[start] == ' ' ||
                        header.value.data[start] == '\t')) {
                    start++;
                }
                while (end > start &&
                       (header.value.data[end - 1] == ' ' ||
                        header.value.data[end - 1] == '\t')) {
                    end--;
                }
                piece.data = header.value.data + start;
                piece.size = end - start;
                if (piece.size == want_token.size &&
                    cfw_strview_equal_nocase(&piece, &want_token,
                                             &peq) == CFW_OK &&
                    peq) {
                    return 1;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* State lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void cfw_hc_state_free(cfw_hc_state_t *st);

static cfw_status_t cfw_hc_state_init(cfw_hc_state_t *st)
{
    cfw_status_t status;

    memset(st, 0, sizeof(*st));
    cfw_fd_init(&st->fd);
    st->pollset = NULL;
    st->tls = NULL;
    st->conn_open = 0;
    st->pending_pos = 0;
    st->input_closed = 0;
    st->auth_port = 0;
    st->auth_tls = 0;
    status = cfw_pollset_create(&st->pollset);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_buffer_init(&st->pending);
    if (status == CFW_OK) {
        status = cfw_buffer_init(&st->auth_host);
    }
    if (status == CFW_OK) {
        status = cfw_buffer_init(&st->tls_session);
    }
    if (status != CFW_OK) {
        cfw_hc_state_free(st);
    }
    return status;
}

/* Saves the TLS session (when present) then drops fd/tls/pending. */
static void cfw_hc_conn_close(cfw_hc_state_t *st)
{
    if (st->tls != NULL) {
        size_t need = 0;
        if (cfw_tls_conn_get_session(st->tls, NULL, 0, &need) == CFW_OK &&
            need != 0) {
            if (cfw_buffer_reserve(&st->tls_session, need) == CFW_OK) {
                size_t written = 0;
                if (cfw_tls_conn_get_session(st->tls,
                                             st->tls_session.data,
                                             st->tls_session.capacity,
                                             &written) == CFW_OK) {
                    st->tls_session.size = written;
                } else {
                    cfw_buffer_clear(&st->tls_session);
                }
            } else {
                cfw_buffer_clear(&st->tls_session);
            }
        } else {
            cfw_buffer_clear(&st->tls_session);
        }
        cfw_tls_conn_destroy(st->tls);
        st->tls = NULL;
    }
    if (st->conn_open) {
        (void)cfw_pollset_remove(st->pollset,
                                 (intptr_t)cfw_fd_get(&st->fd));
        cfw_fd_close(&st->fd);
        cfw_fd_init(&st->fd);
        st->conn_open = 0;
    }
    cfw_buffer_clear(&st->pending);
    st->pending_pos = 0;
    st->input_closed = 0;
}

static void cfw_hc_state_free(cfw_hc_state_t *st)
{
    if (st == NULL) {
        return;
    }
    cfw_hc_conn_close(st);
    if (st->pollset != NULL) {
        cfw_pollset_destroy(st->pollset);
        st->pollset = NULL;
    }
    cfw_buffer_free(&st->pending);
    cfw_buffer_free(&st->auth_host);
    cfw_buffer_free(&st->tls_session);
    memset(st, 0, sizeof(*st));
}

/* ------------------------------------------------------------------ */
/* Socket I/O                                                          */
/* ------------------------------------------------------------------ */

/* Waits until the socket is readable/writable per `events`
 * (CFW_POLL_IN / CFW_POLL_OUT), bounded by the call deadline. */
static cfw_status_t cfw_hc_wait(cfw_hc_state_t *st,
                                const cfw_hc_clock_t *clock,
                                uint32_t events)
{
    cfw_poll_event_t ready[1];
    size_t count = 0;
    cfw_status_t status;

    status = cfw_pollset_modify(st->pollset,
                                (intptr_t)cfw_fd_get(&st->fd), events,
                                NULL);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_pollset_wait(st->pollset,
                              clock->has_deadline ? &clock->deadline
                                                  : NULL,
                              ready, 1, &count);
    if (status == CFW_ERROR_TIMEOUT) {
        return CFW_ERROR_TIMEOUT;
    }
    if (status != CFW_OK) {
        return status;
    }
    /* Event flags (ERR/HUP) are not fatal by themselves: the follow-up
     * I/O attempt reports the real outcome (EOF, error, or data). */
    (void)count;
    (void)ready;
    return CFW_OK;
}

/* Direction mask for a WOULDBLOCK retry: TLS surfaces want_read /
 * want_write hints; cleartext waits on the requested direction. */
static uint32_t cfw_hc_tls_events(const cfw_hc_state_t *st,
                                  int for_write)
{
    uint32_t events = 0;
    if (cfw_tls_conn_want_read(st->tls)) {
        events |= CFW_POLL_IN;
    }
    if (cfw_tls_conn_want_write(st->tls)) {
        events |= CFW_POLL_OUT;
    }
    if (events == 0) {
        events = for_write ? CFW_POLL_OUT : CFW_POLL_IN;
    }
    return events;
}

static cfw_status_t cfw_hc_read(cfw_hc_state_t *st,
                                const cfw_hc_clock_t *clock,
                                void *buffer, size_t size, size_t *done,
                                int *eof)
{
    cfw_status_t status;

    *eof = 0;
    for (;;) {
        if (st->tls != NULL) {
            status = cfw_tls_read(st->tls, buffer, size, done);
        } else {
            status = cfw_net_recv(&st->fd, buffer, size, done);
        }
        if (status == CFW_OK) {
            *eof = (*done == 0);
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        /* Decrypted-but-unread TLS bytes need no socket wait; surface
         * WOULDBLOCK so the caller can drain them without polling. */
        if (st->tls != NULL && cfw_tls_pending(st->tls) != 0) {
            return CFW_ERROR_WOULDBLOCK;
        }
        status = cfw_hc_wait(st, clock,
                             st->tls != NULL
                                 ? cfw_hc_tls_events(st, 0)
                                 : CFW_POLL_IN);
        if (status != CFW_OK) {
            return status;
        }
    }
}

static cfw_status_t cfw_hc_write(cfw_hc_state_t *st,
                                 const cfw_hc_clock_t *clock,
                                 const void *data, size_t size,
                                 size_t *done)
{
    cfw_status_t status;

    for (;;) {
        if (st->tls != NULL) {
            status = cfw_tls_write(st->tls, data, size, done);
        } else {
            status = cfw_net_send(&st->fd, data, size, done);
        }
        if (status == CFW_OK) {
            if (*done == 0) {
                /* A zero-progress write on a nonempty request is a
                 * broken transport, not progress. */
                return CFW_ERROR_IO;
            }
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        status = cfw_hc_wait(st, clock,
                             st->tls != NULL
                                 ? cfw_hc_tls_events(st, 1)
                                 : CFW_POLL_OUT);
        if (status != CFW_OK) {
            return status;
        }
    }
}

/* Writes all of data, waiting on the deadline between partial writes. */
static cfw_status_t cfw_hc_write_all(cfw_hc_state_t *st,
                                     const cfw_hc_clock_t *clock,
                                     const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t off = 0;

    while (off < size) {
        size_t done = 0;
        cfw_status_t status =
            cfw_hc_write(st, clock, p + off, size - off, &done);
        if (status != CFW_OK) {
            return status;
        }
        off += done;
    }
    return CFW_OK;
}

/* Appends received bytes to the pending-input buffer, compacting
 * consumed prefix space first. Sets *eof on orderly shutdown. */
static cfw_status_t cfw_hc_fill(cfw_hc_state_t *st,
                                const cfw_hc_clock_t *clock,
                                int *eof)
{
    char buffer[CFW_HTTP_CLIENT_READ_SIZE];
    size_t got = 0;
    cfw_status_t status;
    cfw_buffer_t *pending = &st->pending;
    size_t *pos = &st->pending_pos;

    for (;;) {
        status = cfw_hc_read(st, clock, buffer, sizeof(buffer), &got,
                             eof);
        if (status == CFW_OK) {
            break;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        /* WOULDBLOCK here means TLS has decrypted bytes pending that
         * need no socket wait; loop back and read them. */
    }

    if (got == 0) {
        return CFW_OK;
    }
    if (*pos != 0) {
        if (*pos == pending->size) {
            cfw_buffer_clear(pending);
            *pos = 0;
        } else if (*pos >= 8192 || *pos >= pending->size / 2) {
            memmove(pending->data, pending->data + *pos,
                    pending->size - *pos);
            pending->size -= *pos;
            *pos = 0;
        }
    }
    return cfw_buffer_append(pending, buffer, got);
}

/* Delivers decoded body bytes to the configured sink/buffer,
 * enforcing the cumulative cap in every mode. */
static cfw_status_t cfw_hc_body_deliver(cfw_hc_body_t *body,
                                        const void *data, size_t size)
{
    if (size == 0) {
        return CFW_OK;
    }
    if (body->total > body->max_body ||
        (uint64_t)size > body->max_body - body->total) {
        return CFW_ERROR_OVERFLOW;
    }
    body->total += (uint64_t)size;
    if (body->sink != NULL) {
        body->sink_started = 1;
        return body->sink(body->sink_user, data, size);
    }
    if (body->buffer != NULL) {
        return cfw_buffer_append(body->buffer, data, size);
    }
    return CFW_OK;
}

/* Drives the response head to completion, skipping interim 1xx
 * responses (except 101, which is returned as the final response).
 * On success `head_view` references the head_reader's storage and
 * `parsed` is filled; the caller must consume it before the next
 * head_reader_reset/free. */
static cfw_status_t cfw_hc_read_head(
    cfw_hc_state_t *st, const cfw_hc_clock_t *clock,
    cfw_http_head_reader_t *reader, cfw_http_response_head_t *parsed)
{
    cfw_status_t status;
    cfw_strview_t head_view;

    for (;;) {
        while (!cfw_http_head_reader_is_complete(reader)) {
            cfw_span_t input;
            size_t consumed = 0;

            if (st->input_closed) {
                return CFW_ERROR_IO;
            }
            status = cfw_hc_fill(st, clock, &st->input_closed);
            if (status != CFW_OK) {
                return status;
            }
            if (st->input_closed && st->pending.size == st->pending_pos) {
                return CFW_ERROR_IO;
            }
            input.data = st->pending.size > st->pending_pos
                             ? st->pending.data + st->pending_pos
                             : NULL;
            input.size = st->pending.size - st->pending_pos;
            status = cfw_http_head_reader_feed(reader, &input, &consumed);
            if (status != CFW_OK) {
                /* Oversized or malformed head input. */
                return status == CFW_ERROR_INVALID_ARGUMENT
                           ? CFW_ERROR_OVERFLOW
                           : status;
            }
            st->pending_pos += consumed;
        }

        status = cfw_http_head_reader_head(reader, &head_view);
        if (status != CFW_OK) {
            return status;
        }
        status = cfw_http_response_head_parse(&head_view, parsed);
        if (status != CFW_OK) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }

        if (parsed->status_code >= 100 && parsed->status_code <= 199 &&
            parsed->status_code != 101) {
            /* Interim response (e.g. 100 Continue): discard and read
             * the next head. Bytes past the interim head remain in
             * `pending` for the next iteration. */
            cfw_http_head_reader_reset(reader);
            continue;
        }
        return CFW_OK;
    }
}

static cfw_status_t cfw_hc_read_body(
    cfw_hc_state_t *st, const cfw_hc_clock_t *clock,
    cfw_hc_body_t *body, const cfw_http_message_framing_t *framing)
{
    cfw_status_t status;

    switch (framing->framing) {
    case CFW_HTTP_BODY_NONE:
        return CFW_OK;

    case CFW_HTTP_BODY_CONTENT_LENGTH: {
        uint64_t remaining = framing->content_length;
        while (remaining > 0) {
            size_t avail = st->pending.size - st->pending_pos;
            size_t take;
            if (avail == 0) {
                if (st->input_closed) {
                    return CFW_ERROR_IO;
                }
                status = cfw_hc_fill(st, clock, &st->input_closed);
                if (status != CFW_OK) {
                    return status;
                }
                continue;
            }
            take = avail < remaining ? avail : (size_t)remaining;
            status = cfw_hc_body_deliver(body, st->pending.data +
                                                   st->pending_pos,
                                         take);
            if (status != CFW_OK) {
                return status;
            }
            st->pending_pos += take;
            remaining -= (uint64_t)take;
        }
        return CFW_OK;
    }

    case CFW_HTTP_BODY_CHUNKED: {
        cfw_http_chunked_decoder_t decoder;
        int finished = 0;
        cfw_http_chunked_decoder_init(&decoder);

        while (!finished) {
            if (st->pending_pos < st->pending.size) {
                cfw_span_t input;
                cfw_span_t output;
                size_t consumed = 0;
                int fin = 0;

                input.data = st->pending.data + st->pending_pos;
                input.size = st->pending.size - st->pending_pos;
                status = cfw_http_chunked_decode(&decoder, &input,
                                                 &output, &consumed,
                                                 &fin);
                if (status != CFW_OK) {
                    return CFW_ERROR_IO;
                }
                st->pending_pos += consumed;
                if (output.size != 0) {
                    status = cfw_hc_body_deliver(body, output.data,
                                                 output.size);
                    if (status != CFW_OK) {
                        return status;
                    }
                }
                if (fin) {
                    finished = 1;
                }
                continue;
            }
            if (st->input_closed) {
                return CFW_ERROR_IO;
            }
            status = cfw_hc_fill(st, clock, &st->input_closed);
            if (status != CFW_OK) {
                return status;
            }
        }
        return CFW_OK;
    }

    case CFW_HTTP_BODY_UNTIL_CLOSE:
    default:
        for (;;) {
            size_t avail = st->pending.size - st->pending_pos;
            if (avail != 0) {
                status = cfw_hc_body_deliver(
                    body, st->pending.data + st->pending_pos, avail);
                if (status != CFW_OK) {
                    return status;
                }
                st->pending_pos = st->pending.size;
            }
            if (st->input_closed) {
                return CFW_OK;
            }
            status = cfw_hc_fill(st, clock, &st->input_closed);
            if (status != CFW_OK) {
                return status;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Connect                                                             */
/* ------------------------------------------------------------------ */

/* Nonblocking connect with deadline: waits for writability then
 * checks SO_ERROR to distinguish success from refusal. On any failure
 * the fd is closed and unregistered so the state stays clean. */
static cfw_status_t cfw_hc_connect_fd(cfw_hc_state_t *st,
                                      const cfw_hc_clock_t *clock,
                                      const cfw_addr_t *addr)
{
    cfw_status_t status;
    int registered = 0;

    status = cfw_net_socket(cfw_addr_is_ipv6(addr) ? AF_INET6 : AF_INET,
                            SOCK_STREAM, 0, &st->fd);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_pollset_add(st->pollset, (intptr_t)cfw_fd_get(&st->fd),
                             CFW_POLL_IN | CFW_POLL_OUT, NULL);
    if (status != CFW_OK) {
        goto fail;
    }
    registered = 1;
    status = cfw_net_connect(&st->fd, addr);
    if (status == CFW_OK) {
        st->conn_open = 1;
        return CFW_OK;
    }
    if (status != CFW_ERROR_WOULDBLOCK) {
        goto fail;
    }
    for (;;) {
        int sock_err = 0;
        status = cfw_hc_wait(st, clock, CFW_POLL_OUT);
        if (status != CFW_OK) {
            goto fail;
        }
        status = cfw_net_so_error(&st->fd, &sock_err);
        if (status != CFW_OK) {
            goto fail;
        }
        if (sock_err == 0) {
            st->conn_open = 1;
            return CFW_OK;
        }
        status = CFW_ERROR_IO;
        goto fail;
    }

fail:
    if (registered) {
        (void)cfw_pollset_remove(st->pollset,
                                 (intptr_t)cfw_fd_get(&st->fd));
    }
    cfw_fd_close(&st->fd);
    cfw_fd_init(&st->fd);
    return status;
}

/* TLS handshake driven by want_read/want_write hints and the
 * deadline. */
static cfw_status_t cfw_hc_handshake(cfw_hc_state_t *st,
                                     const cfw_hc_clock_t *clock)
{
    cfw_status_t status;

    for (;;) {
        status = cfw_tls_handshake(st->tls);
        if (status == CFW_OK) {
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        status = cfw_hc_wait(st, clock, cfw_hc_tls_events(st, 0));
        if (status != CFW_OK) {
            return status;
        }
    }
}

/* Ensures st holds a connection bound to eff's authority. Reuses the
 * open connection on an exact authority match; otherwise closes the
 * old one (saving any TLS session) and connects fresh — resolve,
 * nonblocking connect, optional TLS with SNI + session resumption +
 * hostname verification. Sets *reused when the cached connection was
 * kept. */
static cfw_status_t cfw_hc_ensure_conn(cfw_hc_state_t *st,
                                       const cfw_hc_clock_t *clock,
                                       const cfw_hc_effective_t *eff,
                                       cfw_tls_ctx_t *tls_ctx,
                                       int *reused)
{
    cfw_status_t status;
    cfw_addr_t addr;
    cfw_addr_family_t families[2];
    size_t family_i;
    int want_tls = tls_ctx != NULL;
    cfw_strview_t auth_host;
    cfw_strview_t eff_host;
    int host_equal = 0;

    *reused = 0;
    if (st->conn_open) {
        auth_host.data = (const char *)st->auth_host.data;
        auth_host.size = st->auth_host.size;
        eff_host.data = (const char *)eff->host.data;
        eff_host.size = eff->host.size;
        if (st->auth_port == eff->port && st->auth_tls == want_tls &&
            cfw_strview_equal_nocase(&auth_host, &eff_host,
                                     &host_equal) == CFW_OK &&
            host_equal) {
            *reused = 1;
            return CFW_OK;
        }
    }

    cfw_hc_conn_close(st);

    /* Resolve and connect: try IPv4 then IPv6 so dual-stack and
     * v6-only hosts both work. Each attempt shares the deadline. */
    families[0] = CFW_ADDR_FAMILY_IPV4;
    families[1] = CFW_ADDR_FAMILY_IPV6;
    status = CFW_ERROR_IO;
    for (family_i = 0; family_i < 2; family_i++) {
        cfw_status_t attempt;
        attempt = cfw_addr_resolve(&addr, (const char *)eff->host.data,
                                   eff->port, families[family_i]);
        if (attempt != CFW_OK) {
            continue;
        }
        attempt = cfw_hc_connect_fd(st, clock, &addr);
        if (attempt == CFW_OK) {
            status = CFW_OK;
            break;
        }
        status = attempt;
        if (attempt == CFW_ERROR_TIMEOUT ||
            attempt == CFW_ERROR_UNAVAILABLE) {
            break;
        }
        /* connect_fd already unregistered and closed its fd. */
    }
    if (status != CFW_OK) {
        return status;
    }

    /* Optional TLS layer over the connected socket. */
    if (tls_ctx != NULL) {
        status = cfw_tls_conn_create(tls_ctx, cfw_fd_get(&st->fd),
                                     &st->tls);
        if (status != CFW_OK) {
            return status;
        }
        status = cfw_tls_conn_set_server_name(st->tls,
                                              (const char *)eff->host.data);
        if (status != CFW_OK) {
            return status;
        }
        /* Offer the cached session before the handshake (ADR 0062). */
        if (st->tls_session.size != 0) {
            (void)cfw_tls_conn_set_session(
                st->tls, st->tls_session.data, st->tls_session.size);
        }
        status = cfw_hc_handshake(st, clock);
        if (status != CFW_OK) {
            return status;
        }
    }

    /* Bind the connection to this authority (content bytes only;
     * eff->host.size excludes the NUL terminator). */
    cfw_buffer_clear(&st->auth_host);
    status = cfw_buffer_append(&st->auth_host, eff->host.data,
                               eff->host.size);
    if (status != CFW_OK) {
        return status;
    }
    st->auth_port = eff->port;
    st->auth_tls = want_tls;
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Request head construction                                           */
/* ------------------------------------------------------------------ */

/* Builds the serialized request head for eff. `keep_alive` selects
 * the Connection header value. */
static cfw_status_t cfw_hc_build_head(
    const cfw_hc_effective_t *eff, int keep_alive, cfw_buffer_t *head_buf)
{
    cfw_buffer_t authority;
    cfw_http_header_t *headers_all = NULL;
    size_t total_headers = eff->header_count + 4;
    cfw_strview_t host_value;
    cfw_strview_t ua_value;
    cfw_strview_t cl_value;
    cfw_strview_t conn_value;
    char ua_text[64];
    char cl_text[24];
    int bracket_v6;
    size_t need;
    size_t off;
    size_t i;
    cfw_status_t status = CFW_OK;

    memset(&authority, 0, sizeof(authority));
    bracket_v6 = strchr((const char *)eff->host.data, ':') != NULL &&
                 eff->host.data[0] != '[';
    status = cfw_buffer_append(&authority, eff->host.data,
                               eff->host.size);
    if (status == CFW_OK && bracket_v6) {
        /* Rebuild bracketed: "[host]" — prepend '['. */
        cfw_buffer_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        status = cfw_buffer_append(&tmp, "[", 1);
        if (status == CFW_OK) {
            status = cfw_buffer_append(&tmp, authority.data,
                                       authority.size);
        }
        if (status == CFW_OK) {
            status = cfw_buffer_append(&tmp, "]", 1);
        }
        if (status == CFW_OK) {
            cfw_buffer_free(&authority);
            authority = tmp;
        } else {
            cfw_buffer_free(&tmp);
        }
    }
    if (status == CFW_OK) {
        char port_text[8];
        int plen = snprintf(port_text, sizeof(port_text), ":%u",
                            (unsigned)eff->port);
        if (plen <= 0) {
            status = CFW_ERROR_INTERNAL;
        } else {
            status = cfw_buffer_append(&authority, port_text,
                                       (size_t)plen);
        }
    }
    if (status != CFW_OK) {
        cfw_buffer_free(&authority);
        return status;
    }

    headers_all = (cfw_http_header_t *)malloc(
        total_headers * sizeof(*headers_all));
    if (headers_all == NULL) {
        cfw_buffer_free(&authority);
        return CFW_ERROR_NOMEM;
    }

    host_value.data = (const char *)authority.data;
    host_value.size = authority.size;
    {
        int ua_len = snprintf(ua_text, sizeof(ua_text), "cfw/%s",
                              cfw_get_version_string());
        int cl_len = snprintf(cl_text, sizeof(cl_text), "%llu",
                              (unsigned long long)eff->body_size);
        if (ua_len <= 0 || cl_len <= 0) {
            status = CFW_ERROR_INTERNAL;
            goto head_fail;
        }
        ua_value.data = ua_text;
        ua_value.size = (size_t)ua_len;
        cl_value.data = cl_text;
        cl_value.size = (size_t)cl_len;
    }
    {
        cfw_strview_t hn, un, cn, xn;
        cfw_strview_from_cstr(&hn, "host");
        cfw_strview_from_cstr(&un, "user-agent");
        cfw_strview_from_cstr(&cn, "content-length");
        cfw_strview_from_cstr(&xn, "connection");
        cfw_strview_from_cstr(&conn_value,
                              keep_alive ? "keep-alive" : "close");
        headers_all[0].name = hn;
        headers_all[0].value = host_value;
        headers_all[1].name = un;
        headers_all[1].value = ua_value;
        for (i = 0; i < eff->header_count; i++) {
            headers_all[2 + i] = eff->headers[i];
        }
        headers_all[2 + eff->header_count].name = cn;
        headers_all[2 + eff->header_count].value = cl_value;
        headers_all[3 + eff->header_count].name = xn;
        headers_all[3 + eff->header_count].value = conn_value;
    }

    /* Exact size for the complete head, then one-shot format. */
    {
        cfw_strview_t target_view;
        const char *mname = cfw_http_method_name(eff->method);
        /* "M target HTTP/1.1\r\n" + "\r\n" terminator. */
        target_view.data = (const char *)eff->target.data;
        target_view.size = eff->target.size;
        need = strlen(mname) + 1 + target_view.size + 1 + 8 + 2 + 2;
        for (i = 0; i < total_headers; i++) {
            if (headers_all[i].name.size >
                    SIZE_MAX - headers_all[i].value.size - 4 ||
                need > SIZE_MAX - headers_all[i].name.size -
                                      headers_all[i].value.size - 4) {
                status = CFW_ERROR_OVERFLOW;
                goto head_fail;
            }
            need += headers_all[i].name.size +
                    headers_all[i].value.size + 4;
        }
        status = cfw_buffer_reserve(head_buf, need);
        if (status != CFW_OK) {
            goto head_fail;
        }
        {
            cfw_http_version_t version;
            version.major = 1;
            version.minor = 1;
            status = cfw_http_request_head_format(
                eff->method, &target_view, &version, headers_all,
                total_headers, (char *)head_buf->data,
                head_buf->capacity, &off);
        }
        if (status != CFW_OK) {
            goto head_fail;
        }
        head_buf->size = off;
    }

head_fail:
    free(headers_all);
    cfw_buffer_free(&authority);
    return status;
}

/* ------------------------------------------------------------------ */
/* Single exchange (connect-if-needed, send, receive)                  */
/* ------------------------------------------------------------------ */

/* Performs one request/response exchange on st's connection. Ensures
 * the connection matches eff's authority, sends the request, reads
 * the response head and body. On success appends the raw head to
 * head_out, delivers the body per `body` (which the caller may have
 * configured as discard for redirect hops — see `redirected`), and
 * sets *conn_reusable when the connection negotiated persistence and
 * is cleanly positioned for reuse. `redirected` reports that this
 * response was consumed as an intermediate redirect hop (3xx with a
 * Location while hops remain): its body was drained and discarded.
 *
 * Retries: an exchange failing with CFW_ERROR_IO is retried while the
 * effective method is idempotent and budget remains — a reused
 * connection gets one free stale-connection retry, fresh failures
 * consume `max_retries`. A body sink that already received bytes
 * disables retries (delivery cannot be unwound). */
static cfw_status_t cfw_hc_exchange(
    cfw_hc_state_t *st, const cfw_hc_clock_t *clock,
    const cfw_hc_effective_t *eff, cfw_tls_ctx_t *tls_ctx,
    size_t max_head, unsigned max_retries, unsigned hops_left,
    int keep_alive, cfw_hc_body_t *body, cfw_buffer_t *head_out,
    int *conn_reusable, int *redirected)
{
    cfw_status_t status = CFW_ERROR_INTERNAL;
    cfw_buffer_t head_buf;
    cfw_http_head_reader_t reader;
    cfw_http_response_head_t resp_head;
    cfw_http_message_framing_t framing;
    int head_reader_ready = 0;
    int reused = 0;
    int stale_used = 0;
    unsigned tries = 0;

    memset(&head_buf, 0, sizeof(head_buf));
    *conn_reusable = 0;
    *redirected = 0;

    for (;;) {
        int is_redirect = 0;

        status = cfw_hc_ensure_conn(st, clock, eff, tls_ctx, &reused);
        if (status != CFW_OK) {
            goto out;
        }

        /* Per-attempt reset: drop partial head/body from a failed
         * attempt. A sink that already fired makes retries unsafe —
         * body.sink_started guards that below. */
        cfw_buffer_clear(head_out);
        cfw_buffer_clear(&head_buf);
        if (body->buffer != NULL) {
            cfw_buffer_clear(body->buffer);
        }
        body->total = 0;

        status = cfw_hc_build_head(eff, keep_alive, &head_buf);
        if (status != CFW_OK) {
            goto out;
        }
        status = cfw_hc_write_all(st, clock, head_buf.data,
                                  head_buf.size);
        if (status == CFW_OK && eff->body_size != 0) {
            status = cfw_hc_write_all(st, clock, eff->body,
                                      eff->body_size);
        }
        if (status == CFW_OK) {
            cfw_strview_t head_view;
            status = cfw_http_head_reader_init(&reader, max_head);
            if (status != CFW_OK) {
                goto retry_check;
            }
            head_reader_ready = 1;
            status = cfw_hc_read_head(st, clock, &reader, &resp_head);
            if (status == CFW_OK) {
                status = cfw_http_head_reader_head(&reader, &head_view);
            }
            if (status == CFW_OK) {
                /* Persist the raw head so response_head() can
                 * re-parse it and redirects can resolve Location. */
                status = cfw_buffer_append(head_out, head_view.data,
                                           head_view.size);
            }
            if (status == CFW_OK) {
                status = cfw_http_response_framing(
                    &resp_head, eff->method, &framing);
                if (status != CFW_OK) {
                    status = CFW_ERROR_IO;
                }
            }
            if (status == CFW_OK) {
                cfw_strview_t location;
                /* A 3xx with a Location while hops remain is an
                 * intermediate hop: drain its body bounded but never
                 * deliver it to the caller's sink/buffer. */
                is_redirect =
                    hops_left > 0 &&
                    (resp_head.status_code == 301 ||
                     resp_head.status_code == 302 ||
                     resp_head.status_code == 303 ||
                     resp_head.status_code == 307 ||
                     resp_head.status_code == 308) &&
                    cfw_hc_find_header(&resp_head.headers, "location",
                                       &location);
                {
                    /* Redirect hops never reach the caller's sink
                     * (it cannot be unwound), but the body still
                     * lands in the hop buffer: discarded when the
                     * redirect is followed, preserved when the 3xx
                     * turns out to be the final response (unresolvable
                     * Location). */
                    cfw_status_t (*save_sink)(void *, const void *,
                                              size_t) = body->sink;
                    void *save_user = body->sink_user;
                    if (is_redirect) {
                        body->sink = NULL;
                        body->sink_user = NULL;
                    }
                    status = cfw_hc_read_body(st, clock, body,
                                              &framing);
                    body->sink = save_sink;
                    body->sink_user = save_user;
                }
            }
            if (status == CFW_OK) {
                /* Persistence decision (RFC 9112): self-terminating
                 * framing, no Connection: close, HTTP/1.1+ or explicit
                 * keep-alive, no leftover bytes, no EOF seen. */
                int http11 = resp_head.version.major > 1 ||
                             (resp_head.version.major == 1 &&
                              resp_head.version.minor >= 1);
                if (keep_alive &&
                    framing.framing != CFW_HTTP_BODY_UNTIL_CLOSE &&
                    !st->input_closed &&
                    st->pending_pos == st->pending.size &&
                    resp_head.status_code != 101 &&
                    !cfw_hc_header_has_token(&resp_head.headers,
                                             "connection", "close") &&
                    (http11 ||
                     cfw_hc_header_has_token(&resp_head.headers,
                                             "connection",
                                             "keep-alive"))) {
                    *conn_reusable = 1;
                }
                *redirected = is_redirect;
                if (head_reader_ready) {
                    cfw_http_head_reader_free(&reader);
                    head_reader_ready = 0;
                }
                goto out;
            }
        }

    retry_check:
        if (head_reader_ready) {
            cfw_http_head_reader_free(&reader);
            head_reader_ready = 0;
        }
        /* Retry policy: IO failures only, idempotent methods only,
         * and never after a body sink delivered bytes (an unwound
         * partial stream cannot be re-sent). A connection reused
         * from the session gets one free stale-connection retry;
         * further attempts consume max_retries. */
        if (status == CFW_ERROR_IO && eff->idempotent &&
            !body->sink_started) {
            int allow = 0;
            if (reused && !stale_used) {
                stale_used = 1;
                allow = 1;
            } else if (tries < max_retries) {
                tries++;
                allow = 1;
            }
            if (allow) {
                cfw_hc_conn_close(st);
                reused = 0;
                continue;
            }
        }
        cfw_hc_conn_close(st);
        goto out;
    }

out:
    if (head_reader_ready) {
        cfw_http_head_reader_free(&reader);
    }
    cfw_buffer_free(&head_buf);
    return status;
}

/* ------------------------------------------------------------------ */
/* Redirect resolution (RFC 7231 Location + RFC 3986 merge)            */
/* ------------------------------------------------------------------ */

/* RFC 3986 5.2.4 dot-segment removal on a path buffer (must start
 * with '/' or be empty). */
static cfw_status_t cfw_hc_remove_dot_segments(cfw_buffer_t *path)
{
    cfw_buffer_t out;
    size_t i = 0;
    cfw_status_t status;

    memset(&out, 0, sizeof(out));
    status = cfw_buffer_init(&out);
    if (status != CFW_OK) {
        return status;
    }
    while (i < path->size) {
        size_t seg_start = i;
        size_t seg_end;
        const uint8_t *seg;
        const uint8_t *content;
        size_t seg_len;
        size_t clen;
        int last;

        /* Extract one segment: optional leading '/' plus the run up
         * to the next '/'. */
        if (path->data[i] == '/') {
            i++;
        }
        while (i < path->size && path->data[i] != '/') {
            i++;
        }
        seg_end = i;
        seg = path->data + seg_start;
        seg_len = seg_end - seg_start;
        content = seg;
        clen = seg_len;
        if (clen != 0 && content[0] == '/') {
            content++;
            clen--;
        }
        last = (seg_end == path->size);

        if (clen == 1 && content[0] == '.') {
            /* "." segment: drop it; a trailing one still yields '/'. */
            if (last && out.size != 0 &&
                out.data[out.size - 1] != '/') {
                status = cfw_buffer_append(&out, "/", 1);
                if (status != CFW_OK) {
                    cfw_buffer_free(&out);
                    return status;
                }
            }
            continue;
        }
        if (clen == 2 && content[0] == '.' && content[1] == '.') {
            /* ".." segment: drop the last output segment (back to
             * and including its leading '/'). */
            size_t j = out.size;
            while (j > 0 && out.data[j - 1] != '/') {
                j--;
            }
            out.size = (j == 0) ? 0 : j - 1;
            if (last && out.size != 0 &&
                out.data[out.size - 1] != '/') {
                status = cfw_buffer_append(&out, "/", 1);
                if (status != CFW_OK) {
                    cfw_buffer_free(&out);
                    return status;
                }
            }
            continue;
        }
        status = cfw_buffer_append(&out, seg, seg_len);
        if (status != CFW_OK) {
            cfw_buffer_free(&out);
            return status;
        }
    }
    if (out.size == 0) {
        status = cfw_buffer_append(&out, "/", 1);
        if (status != CFW_OK) {
            cfw_buffer_free(&out);
            return status;
        }
    }
    cfw_buffer_free(path);
    *path = out;
    return CFW_OK;
}

/* Resolves a Location header value against eff and rewrites eff's
 * host/port/target in place. Returns CFW_OK when the redirect may be
 * followed; CFW_ERROR_INVALID_ARGUMENT when it must not be (caller
 * returns the 3xx response unchanged). Never crosses the http/https
 * boundary implied by cur_tls and rejects userinfo authorities. */
static cfw_status_t cfw_hc_resolve_redirect(cfw_hc_effective_t *eff,
                                            int cur_tls,
                                            const cfw_strview_t *location)
{
    const char *p = location->data;
    size_t n = location->size;
    size_t i;
    size_t scheme_end = (size_t)-1;
    int https;
    const char *rest;
    size_t rest_len;
    const char *auth = NULL;
    size_t auth_len = 0;
    const char *pathq = NULL;
    size_t pathq_len = 0;
    int have_auth = 0;
    cfw_buffer_t new_host;
    cfw_buffer_t new_target;
    cfw_strview_t cur_path;
    cfw_status_t status;

    memset(&new_host, 0, sizeof(new_host));
    memset(&new_target, 0, sizeof(new_target));

    /* Strip the fragment. */
    for (i = 0; i < n; i++) {
        if (p[i] == '#') {
            n = i;
            break;
        }
    }
    if (n == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Locate "://" for an absolute URI. */
    for (i = 0; i + 2 < n; i++) {
        if (p[i] == ':' && p[i + 1] == '/' && p[i + 2] == '/') {
            scheme_end = i;
            break;
        }
    }

    https = cur_tls;
    if (scheme_end != (size_t)-1) {
        cfw_strview_t scheme;
        cfw_strview_t http_s, https_s;
        int is_http = 0, is_https = 0, eq = 0;
        scheme.data = p;
        scheme.size = scheme_end;
        cfw_strview_from_cstr(&http_s, "http");
        cfw_strview_from_cstr(&https_s, "https");
        if (cfw_strview_equal_nocase(&scheme, &http_s, &eq) == CFW_OK &&
            eq) {
            is_http = 1;
        } else if (eq = 0,
                   cfw_strview_equal_nocase(&scheme, &https_s, &eq) ==
                       CFW_OK &&
                   eq) {
            is_https = 1;
        }
        if (!is_http && !is_https) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        https = is_https;
        /* Never cross the http/https boundary implied by tls_ctx. */
        if (https != cur_tls) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        rest = p + scheme_end + 3;
        rest_len = n - (size_t)(rest - p);
        have_auth = 1;
    } else if (n >= 2 && p[0] == '/' && p[1] == '/') {
        /* Scheme-relative "//authority/path". */
        rest = p + 2;
        rest_len = n - 2;
        have_auth = 1;
    } else {
        rest = p;
        rest_len = n;
    }

    /* Split authority from path for the authority forms: the
     * authority ends at the first '/', '?', or end of input. */
    if (have_auth) {
        size_t j;
        for (j = 0; j < rest_len; j++) {
            if (rest[j] == '/' || rest[j] == '?') {
                break;
            }
        }
        auth = rest;
        auth_len = j;
        pathq = rest + j;
        pathq_len = rest_len - j; /* starts with '/' or '?' or empty */
        if (pathq_len == 0) {
            pathq = "/";
            pathq_len = 1;
        }
    } else {
        pathq = p;
        pathq_len = n;
    }

    if (have_auth) {
        /* Reject userinfo and empty authority. */
        size_t k;
        const char *hp;
        size_t hn;
        uint32_t port_num = 0;
        int have_port = 0;
        for (k = 0; k < auth_len; k++) {
            unsigned char c = (unsigned char)auth[k];
            if (c == '@' || c <= 0x20 || c == 0x7f) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
        if (auth_len == 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        /* IPv6 literal: "[::1]" optionally followed by ":port". */
        if (auth[0] == '[') {
            const char *close = memchr(auth, ']', auth_len);
            if (close == NULL) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
            hp = auth;
            hn = (size_t)(close - auth) + 1;
            if (hn + 1 < auth_len && close[1] == ':') {
                const char *pd = close + 2;
                size_t pdn = auth_len - hn - 1;
                size_t d;
                if (pdn == 0) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                for (d = 0; d < pdn; d++) {
                    if (pd[d] < '0' || pd[d] > '9') {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                    port_num = port_num * 10u + (uint32_t)(pd[d] - '0');
                    if (port_num > 65535u) {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                }
                have_port = 1;
            } else if (hn != auth_len) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        } else {
            const char *colon = memchr(auth, ':', auth_len);
            if (colon != NULL) {
                const char *pd = colon + 1;
                size_t pdn = auth_len - (size_t)(colon - auth) - 1;
                size_t d;
                hp = auth;
                hn = (size_t)(colon - auth);
                if (hn == 0 || pdn == 0) {
                    return CFW_ERROR_INVALID_ARGUMENT;
                }
                for (d = 0; d < pdn; d++) {
                    if (pd[d] < '0' || pd[d] > '9') {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                    port_num = port_num * 10u + (uint32_t)(pd[d] - '0');
                    if (port_num > 65535u) {
                        return CFW_ERROR_INVALID_ARGUMENT;
                    }
                }
                have_port = 1;
            } else {
                hp = auth;
                hn = auth_len;
            }
        }
        if (hn == 0 || (have_port && port_num == 0)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        status = cfw_hc_buf_set(&new_host, hp, hn);
        if (status != CFW_OK) {
            goto fail;
        }
        eff->port = have_port ? (uint16_t)port_num
                              : (https ? 443u : 80u);
    }

    /* Resolve the path+query into new_target. */
    status = cfw_buffer_init(&new_target);
    if (status != CFW_OK) {
        goto fail;
    }

    /* Current path (target without query) for relative merges. */
    cur_path.data = (const char *)eff->target.data;
    cur_path.size = eff->target.size;
    for (i = 0; i < cur_path.size; i++) {
        if (cur_path.data[i] == '?') {
            cur_path.size = i;
            break;
        }
    }

    if (pathq_len != 0 && pathq[0] == '/') {
        /* Absolute path (or "/" alone). */
        status = cfw_buffer_append(&new_target, pathq, pathq_len);
        if (status != CFW_OK) {
            goto fail;
        }
    } else if (pathq_len != 0 && pathq[0] == '?') {
        /* Query-only: keep the current path. */
        status = cfw_buffer_append(&new_target, cur_path.data,
                                   cur_path.size);
        if (status == CFW_OK) {
            status = cfw_buffer_append(&new_target, pathq, pathq_len);
        }
        if (status != CFW_OK) {
            goto fail;
        }
    } else {
        /* Relative reference: merge with the current path's
         * directory, then remove dot segments. */
        cfw_buffer_t merged;
        size_t dir_end = 0;
        size_t qmark = pathq_len;
        memset(&merged, 0, sizeof(merged));
        status = cfw_buffer_init(&merged);
        if (status != CFW_OK) {
            goto fail;
        }
        for (i = 0; i < pathq_len; i++) {
            if (pathq[i] == '?') {
                qmark = i;
                break;
            }
        }
        /* Directory = current path through its last '/'. */
        for (i = 0; i < cur_path.size; i++) {
            if (cur_path.data[i] == '/') {
                dir_end = i + 1;
            }
        }
        status = cfw_buffer_append(&merged, cur_path.data, dir_end);
        if (status == CFW_OK && qmark != 0) {
            status = cfw_buffer_append(&merged, pathq, qmark);
        }
        if (status != CFW_OK) {
            cfw_buffer_free(&merged);
            goto fail;
        }
        /* Ensure leading '/'. */
        if (merged.size == 0 || merged.data[0] != '/') {
            cfw_buffer_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            status = cfw_buffer_append(&tmp, "/", 1);
            if (status == CFW_OK) {
                status = cfw_buffer_append(&tmp, merged.data,
                                           merged.size);
            }
            cfw_buffer_free(&merged);
            if (status != CFW_OK) {
                goto fail;
            }
            merged = tmp;
        }
        status = cfw_hc_remove_dot_segments(&merged);
        if (status != CFW_OK) {
            cfw_buffer_free(&merged);
            goto fail;
        }
        status = cfw_buffer_append(&new_target, merged.data,
                                   merged.size);
        cfw_buffer_free(&merged);
        if (status == CFW_OK && qmark != pathq_len) {
            status = cfw_buffer_append(&new_target, pathq + qmark,
                                       pathq_len - qmark);
        }
        if (status != CFW_OK) {
            goto fail;
        }
    }
    if (new_target.size == 0) {
        status = cfw_buffer_append(&new_target, "/", 1);
        if (status != CFW_OK) {
            goto fail;
        }
    }

    /* Commit into eff. */
    if (have_auth) {
        cfw_buffer_free(&eff->host);
        memset(&eff->host, 0, sizeof(eff->host));
        eff->host = new_host;
        memset(&new_host, 0, sizeof(new_host));
    }
    cfw_buffer_free(&eff->target);
    memset(&eff->target, 0, sizeof(eff->target));
    eff->target = new_target;
    memset(&new_target, 0, sizeof(new_target));

    cfw_buffer_free(&new_host);
    cfw_buffer_free(&new_target);
    return CFW_OK;

fail:
    cfw_buffer_free(&new_host);
    cfw_buffer_free(&new_target);
    return status == CFW_OK ? CFW_ERROR_INVALID_ARGUMENT : status;
}

/* ------------------------------------------------------------------ */
/* Effective request lifecycle                                         */
/* ------------------------------------------------------------------ */

static void cfw_hc_effective_free(cfw_hc_effective_t *eff)
{
    free(eff->headers_owned);
    eff->headers_owned = NULL;
    cfw_buffer_free(&eff->host);
    cfw_buffer_free(&eff->target);
    memset(eff, 0, sizeof(*eff));
}

static cfw_status_t cfw_hc_effective_init(cfw_hc_effective_t *eff,
                                          const cfw_http_client_request_t *request)
{
    memset(eff, 0, sizeof(*eff));
    eff->method = request->method;
    eff->port = request->port;
    eff->headers = request->headers;
    eff->header_count = request->header_count;
    eff->body = request->body;
    eff->body_size = request->body_size;
    eff->idempotent = cfw_hc_method_idempotent(request->method);

    if (cfw_buffer_init(&eff->host) != CFW_OK ||
        cfw_buffer_init(&eff->target) != CFW_OK) {
        cfw_hc_effective_free(eff);
        return CFW_ERROR_NOMEM;
    }
    {
        cfw_status_t s1 =
            cfw_hc_buf_set(&eff->host, request->host,
                           strlen(request->host));
        cfw_status_t s2 =
            s1 == CFW_OK
                ? cfw_hc_buf_set(&eff->target, request->target,
                                 strlen(request->target))
                : s1;
        if (s2 != CFW_OK) {
            cfw_hc_effective_free(eff);
            return s2;
        }
    }
    return CFW_OK;
}

/* Drops Authorization/Cookie headers when a redirect changes
 * authority (they must not leak across origins). */
static cfw_status_t cfw_hc_filter_sensitive_headers(cfw_hc_effective_t *eff)
{
    cfw_http_header_t *copy;
    size_t kept = 0;
    size_t i;
    cfw_strview_t auth_name, cookie_name;
    int eq;

    free(eff->headers_owned);
    eff->headers_owned = NULL;

    copy = (cfw_http_header_t *)malloc(
        (eff->header_count != 0 ? eff->header_count : 1) *
        sizeof(*copy));
    if (copy == NULL) {
        return CFW_ERROR_NOMEM;
    }
    cfw_strview_from_cstr(&auth_name, "authorization");
    cfw_strview_from_cstr(&cookie_name, "cookie");
    for (i = 0; i < eff->header_count; i++) {
        eq = 0;
        if ((cfw_strview_equal_nocase(&eff->headers[i].name,
                                      &auth_name, &eq) == CFW_OK &&
             eq) ||
            (eq = 0,
             cfw_strview_equal_nocase(&eff->headers[i].name,
                                      &cookie_name, &eq) == CFW_OK &&
             eq)) {
            continue;
        }
        copy[kept++] = eff->headers[i];
    }
    eff->headers_owned = copy;
    eff->headers = copy;
    eff->header_count = kept;
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

static cfw_status_t cfw_hc_validate_request(
    const cfw_http_client_request_t *request)
{
    size_t i;

    if (request == NULL || request->host == NULL ||
        request->host[0] == '\0' || request->port == 0 ||
        request->method == CFW_HTTP_METHOD_UNKNOWN ||
        request->target == NULL || request->target[0] == '\0' ||
        (request->body_size != 0 && request->body == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (cfw_http_method_name(request->method) == NULL ||
        request->timeout.nanoseconds < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Reject framework-managed header names up front (case-insensitive). */
    for (i = 0; i < request->header_count; i++) {
        if (request->headers[i].name.data == NULL ||
            cfw_client_name_is_forbidden(&request->headers[i].name)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    return CFW_OK;
}

/* Shared perform engine: `persistent` selects keep-alive semantics
 * (session) vs Connection: close (one-shot). */
static cfw_status_t cfw_hc_perform(cfw_hc_state_t *st,
                                   const cfw_http_client_request_t *request,
                                   cfw_http_client_response_t *response,
                                   int persistent)
{
    cfw_hc_effective_t eff;
    cfw_hc_clock_t clock;
    cfw_status_t status;
    cfw_http_client_response_t *out = response;
    cfw_http_client_response_t discard;
    cfw_buffer_t hop_head;
    cfw_buffer_t hop_body;
    uint64_t max_body;
    size_t max_head;
    cfw_duration_t timeout;
    unsigned hops;
    int hops_left;

    status = cfw_hc_validate_request(request);
    if (status != CFW_OK) {
        return status;
    }

    if (out == NULL) {
        memset(&discard, 0, sizeof(discard));
        out = &discard;
    } else {
        memset(out, 0, sizeof(*out));
    }
    memset(&hop_head, 0, sizeof(hop_head));
    memset(&hop_body, 0, sizeof(hop_body));

    status = cfw_hc_effective_init(&eff, request);
    if (status != CFW_OK) {
        goto done_no_resp;
    }

    timeout = request->timeout;
    if (timeout.nanoseconds == 0) {
        status = cfw_duration_from_millis(CFW_HTTP_CLIENT_DEFAULT_TIMEOUT_MS,
                                          &timeout);
        if (status != CFW_OK) {
            goto done_eff;
        }
    }
    status = cfw_time_now(&clock.deadline);
    if (status != CFW_OK) {
        goto done_eff;
    }
    {
        cfw_timepoint_t deadline;
        status = cfw_timepoint_add(&clock.deadline, &timeout, &deadline);
        if (status != CFW_OK) {
            goto done_eff;
        }
        clock.deadline = deadline;
        clock.has_deadline = 1;
    }

    max_head = request->max_head_size != 0
                   ? request->max_head_size
                   : CFW_HTTP_CLIENT_DEFAULT_MAX_HEAD;
    max_body = request->max_body_size != 0
                   ? request->max_body_size
                   : CFW_HTTP_CLIENT_DEFAULT_MAX_BODY;
    hops = request->max_redirects;
    if (hops > CFW_HTTP_CLIENT_MAX_REDIRECTS) {
        hops = CFW_HTTP_CLIENT_MAX_REDIRECTS;
    }

    status = cfw_buffer_init(&hop_head);
    if (status != CFW_OK) {
        goto done_eff;
    }
    status = cfw_buffer_init(&hop_body);
    if (status != CFW_OK) {
        goto done_eff;
    }
    hops_left = (int)hops;

    for (;;) {
        cfw_hc_body_t body;
        int conn_reusable = 0;
        int redirected = 0;
        cfw_http_response_head_t resp_head;
        cfw_strview_t head_view;
        cfw_strview_t location;
        int has_location;

        memset(&body, 0, sizeof(body));
        body.buffer = &hop_body;
        body.sink = request->body_sink;
        body.sink_user = request->body_sink_user;
        body.max_body = max_body;

        status = cfw_hc_exchange(st, &clock, &eff, request->tls_ctx,
                                 max_head,
                                 request->max_retries >
                                         CFW_HTTP_CLIENT_MAX_RETRIES
                                     ? CFW_HTTP_CLIENT_MAX_RETRIES
                                     : request->max_retries,
                                 (unsigned)hops_left, persistent,
                                 &body, &hop_head, &conn_reusable,
                                 &redirected);
        if (status != CFW_OK) {
            goto done_eff;
        }
        if (!conn_reusable) {
            cfw_hc_conn_close(st);
        }

        /* Re-parse the stored head for redirect decisions; views are
         * stable (they reference hop_head). */
        head_view.data = (const char *)hop_head.data;
        head_view.size = hop_head.size;
        status = cfw_http_response_head_parse(&head_view, &resp_head);
        if (status != CFW_OK) {
            status = CFW_ERROR_INTERNAL;
            goto done_eff;
        }
        has_location =
            cfw_hc_find_header(&resp_head.headers, "location",
                               &location);

        if (redirected && has_location) {
            /* Resolve Location against the effective request. On
             * success eff is rewritten for the next hop; on failure
             * the 3xx response itself is the final answer. */
            cfw_buffer_t prev_host;
            uint16_t prev_port = eff.port;
            int resolve_ok;

            memset(&prev_host, 0, sizeof(prev_host));
            resolve_ok = cfw_buffer_append(&prev_host, eff.host.data,
                                           eff.host.size) == CFW_OK;
            status = cfw_hc_resolve_redirect(
                &eff, request->tls_ctx != NULL, &location);
            if (status == CFW_OK && resolve_ok) {
                cfw_strview_t a, b;
                int eq = 0;
                int auth_changed;
                a.data = (const char *)prev_host.data;
                a.size = prev_host.size;
                b.data = (const char *)eff.host.data;
                b.size = eff.host.size;
                auth_changed =
                    prev_port != eff.port ||
                    (cfw_strview_equal_nocase(&a, &b, &eq) == CFW_OK &&
                     !eq);
                if (auth_changed) {
                    /* Never leak Authorization/Cookie across origins. */
                    status = cfw_hc_filter_sensitive_headers(&eff);
                }
            }
            cfw_buffer_free(&prev_host);
            if (status == CFW_OK && resolve_ok) {
                /* Method rewrite: 303 always becomes GET; 301/302
                 * rewrite POST to GET (browser convention). */
                if (resp_head.status_code == 303 ||
                    ((resp_head.status_code == 301 ||
                      resp_head.status_code == 302) &&
                     eff.method == CFW_HTTP_METHOD_POST)) {
                    eff.method = CFW_HTTP_METHOD_GET;
                    eff.body = NULL;
                    eff.body_size = 0;
                    eff.idempotent = 1;
                }
                hops_left--;
                cfw_buffer_clear(&hop_head);
                cfw_buffer_clear(&hop_body);
                continue;
            }
            /* Unresolvable Location or header-filter failure: fall
             * through and return the 3xx response as final. */
            status = CFW_OK;
        }

        /* Final response: move scratch buffers into the output. */
        cfw_buffer_free(&out->head);
        memset(&out->head, 0, sizeof(out->head));
        out->head = hop_head;
        memset(&hop_head, 0, sizeof(hop_head));
        cfw_buffer_free(&out->body);
        memset(&out->body, 0, sizeof(out->body));
        out->body = hop_body;
        memset(&hop_body, 0, sizeof(hop_body));
        status = CFW_OK;
        break;
    }

done_eff:
    cfw_buffer_free(&hop_head);
    cfw_buffer_free(&hop_body);
    cfw_hc_effective_free(&eff);
done_no_resp:
    if (out == &discard) {
        cfw_http_client_response_free(&discard);
    } else if (status != CFW_OK && response != NULL) {
        /* Failure: discard partial results but keep buffers valid. */
        cfw_buffer_clear(&response->head);
        cfw_buffer_clear(&response->body);
    }
    return status;
}

cfw_status_t cfw_http_client_request(
    const cfw_http_client_request_t *request,
    cfw_http_client_response_t *response)
{
    cfw_hc_state_t st;
    cfw_status_t status;

    status = cfw_hc_state_init(&st);
    if (status != CFW_OK) {
        if (response != NULL) {
            memset(response, 0, sizeof(*response));
        }
        return status;
    }
    /* One-shot: Connection: close, no reuse. */
    status = cfw_hc_perform(&st, request, response, 0);
    cfw_hc_state_free(&st);
    return status;
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

struct cfw_http_client_session {
    cfw_hc_state_t st;
};

cfw_status_t cfw_http_client_session_create(
    cfw_http_client_session_t **session)
{
    cfw_http_client_session_t *s;
    cfw_status_t status;

    if (session == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    *session = NULL;
    s = (cfw_http_client_session_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return CFW_ERROR_NOMEM;
    }
    status = cfw_hc_state_init(&s->st);
    if (status != CFW_OK) {
        free(s);
        return status;
    }
    *session = s;
    return CFW_OK;
}

void cfw_http_client_session_close(cfw_http_client_session_t *session)
{
    if (session == NULL) {
        return;
    }
    cfw_hc_conn_close(&session->st);
}

void cfw_http_client_session_destroy(cfw_http_client_session_t *session)
{
    if (session == NULL) {
        return;
    }
    cfw_hc_state_free(&session->st);
    free(session);
}

cfw_status_t cfw_http_client_session_perform(
    cfw_http_client_session_t *session,
    const cfw_http_client_request_t *request,
    cfw_http_client_response_t *response)
{
    if (session == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Keep-alive semantics: Connection: keep-alive, reuse allowed. */
    return cfw_hc_perform(&session->st, request, response, 1);
}
