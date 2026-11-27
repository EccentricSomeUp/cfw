/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/http_client.h>

#if !CFW_PLATFORM_WINDOWS
    #include <cfw/pollset.h>

    #include <cfw/net.h>
    #include <cfw/span.h>
    #include <cfw/version.h>

    #include <netinet/in.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
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

#if CFW_PLATFORM_WINDOWS

cfw_status_t cfw_http_client_request(
    const cfw_http_client_request_t *request,
    cfw_http_client_response_t *response)
{
    (void)request;
    (void)response;
    return CFW_ERROR_UNAVAILABLE;
}

#else /* POSIX */

#define CFW_HTTP_CLIENT_READ_SIZE 16384u
#define CFW_HTTP_CLIENT_DEFAULT_TIMEOUT_MS 30000ll
#define CFW_HTTP_CLIENT_DEFAULT_MAX_HEAD (64u * 1024u)
#define CFW_HTTP_CLIENT_DEFAULT_MAX_BODY (16ull * 1024ull * 1024ull)

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

typedef struct cfw_client {
    cfw_fd_t fd;
    cfw_tls_conn_t *tls;
    cfw_pollset_t *pollset;
    cfw_timepoint_t deadline;
    int has_deadline;
} cfw_client_t;

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

/* Waits until the socket is readable/writable per `events`
 * (CFW_POLL_IN / CFW_POLL_OUT), bounded by the request deadline. */
static cfw_status_t cfw_client_wait(cfw_client_t *client, uint32_t events)
{
    cfw_poll_event_t ready[1];
    size_t count = 0;
    cfw_status_t status;

    status = cfw_pollset_modify(client->pollset,
                                (intptr_t)cfw_fd_get(&client->fd), events,
                                NULL);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_pollset_wait(client->pollset,
                              client->has_deadline ? &client->deadline
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
static uint32_t cfw_client_tls_events(const cfw_client_t *client,
                                      int for_write)
{
    uint32_t events = 0;
    if (cfw_tls_conn_want_read(client->tls)) {
        events |= CFW_POLL_IN;
    }
    if (cfw_tls_conn_want_write(client->tls)) {
        events |= CFW_POLL_OUT;
    }
    if (events == 0) {
        events = for_write ? CFW_POLL_OUT : CFW_POLL_IN;
    }
    return events;
}

static cfw_status_t cfw_client_read(cfw_client_t *client, void *buffer,
                                    size_t size, size_t *done,
                                    int *eof)
{
    cfw_status_t status;

    *eof = 0;
    for (;;) {
        if (client->tls != NULL) {
            status = cfw_tls_read(client->tls, buffer, size, done);
        } else {
            status = cfw_net_recv(&client->fd, buffer, size, done);
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
        if (client->tls != NULL && cfw_tls_pending(client->tls) != 0) {
            return CFW_ERROR_WOULDBLOCK;
        }
        status = cfw_client_wait(client,
                                 client->tls != NULL
                                     ? cfw_client_tls_events(client, 0)
                                     : CFW_POLL_IN);
        if (status != CFW_OK) {
            return status;
        }
    }
}

static cfw_status_t cfw_client_write(cfw_client_t *client,
                                     const void *data, size_t size,
                                     size_t *done)
{
    cfw_status_t status;

    for (;;) {
        if (client->tls != NULL) {
            status = cfw_tls_write(client->tls, data, size, done);
        } else {
            status = cfw_net_send(&client->fd, data, size, done);
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
        status = cfw_client_wait(client,
                                 client->tls != NULL
                                     ? cfw_client_tls_events(client, 1)
                                     : CFW_POLL_OUT);
        if (status != CFW_OK) {
            return status;
        }
    }
}

/* Writes all of data, waiting on the deadline between partial writes. */
static cfw_status_t cfw_client_write_all(cfw_client_t *client,
                                         const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t off = 0;

    while (off < size) {
        size_t done = 0;
        cfw_status_t status =
            cfw_client_write(client, p + off, size - off, &done);
        if (status != CFW_OK) {
            return status;
        }
        off += done;
    }
    return CFW_OK;
}

/* Appends received bytes to the pending-input buffer, compacting
 * consumed prefix space first. Sets *eof on orderly shutdown. */
static cfw_status_t cfw_client_fill(cfw_client_t *client,
                                    cfw_buffer_t *pending, size_t *pos,
                                    int *eof)
{
    char buffer[CFW_HTTP_CLIENT_READ_SIZE];
    size_t got = 0;
    cfw_status_t status;

    for (;;) {
        status = cfw_client_read(client, buffer, sizeof(buffer), &got,
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

/* Appends decoded body bytes enforcing the configured cap. */
static cfw_status_t cfw_client_body_append(
    cfw_http_client_response_t *response, const void *data, size_t size,
    uint64_t max_body)
{
    if (size == 0) {
        return CFW_OK;
    }
    if (response->body.size > max_body ||
        (uint64_t)size > max_body - (uint64_t)response->body.size) {
        return CFW_ERROR_OVERFLOW;
    }
    return cfw_buffer_append(&response->body, data, size);
}

/* Drives the response head to completion, skipping interim 1xx
 * responses (except 101, which is returned as the final response).
 * On success `head_view` references the head_reader's storage and
 * `parsed` is filled; the caller must consume it before the next
 * head_reader_reset/free. */
static cfw_status_t cfw_client_read_head(
    cfw_client_t *client, cfw_http_head_reader_t *reader,
    cfw_buffer_t *pending, size_t *pos, int *input_closed,
    cfw_strview_t *head_view, cfw_http_response_head_t *parsed)
{
    cfw_status_t status;

    for (;;) {
        while (!cfw_http_head_reader_is_complete(reader)) {
            cfw_span_t input;
            size_t consumed = 0;

            if (*input_closed) {
                return CFW_ERROR_IO;
            }
            status = cfw_client_fill(client, pending, pos, input_closed);
            if (status != CFW_OK) {
                return status;
            }
            if (*input_closed && pending->size == *pos) {
                return CFW_ERROR_IO;
            }
            input.data = pending->size > *pos
                             ? pending->data + *pos
                             : NULL;
            input.size = pending->size - *pos;
            status = cfw_http_head_reader_feed(reader, &input, &consumed);
            if (status != CFW_OK) {
                /* Oversized or malformed head input. */
                return status == CFW_ERROR_INVALID_ARGUMENT
                           ? CFW_ERROR_OVERFLOW
                           : status;
            }
            *pos += consumed;
        }

        status = cfw_http_head_reader_head(reader, head_view);
        if (status != CFW_OK) {
            return status;
        }
        status = cfw_http_response_head_parse(head_view, parsed);
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

static cfw_status_t cfw_client_read_body(
    cfw_client_t *client, cfw_http_client_response_t *response,
    cfw_buffer_t *pending, size_t *pos, int *input_closed,
    const cfw_http_message_framing_t *framing, uint64_t max_body)
{
    cfw_status_t status;

    switch (framing->framing) {
    case CFW_HTTP_BODY_NONE:
        return CFW_OK;

    case CFW_HTTP_BODY_CONTENT_LENGTH: {
        uint64_t remaining = framing->content_length;
        while (remaining > 0) {
            size_t avail = pending->size - *pos;
            size_t take;
            if (avail == 0) {
                if (*input_closed) {
                    return CFW_ERROR_IO;
                }
                status = cfw_client_fill(client, pending, pos,
                                         input_closed);
                if (status != CFW_OK) {
                    return status;
                }
                continue;
            }
            take = avail < remaining ? avail : (size_t)remaining;
            status = cfw_client_body_append(response,
                                            pending->data + *pos, take,
                                            max_body);
            if (status != CFW_OK) {
                return status;
            }
            *pos += take;
            remaining -= (uint64_t)take;
        }
        return CFW_OK;
    }

    case CFW_HTTP_BODY_CHUNKED: {
        cfw_http_chunked_decoder_t decoder;
        int finished = 0;
        cfw_http_chunked_decoder_init(&decoder);

        while (!finished) {
            if (*pos < pending->size) {
                cfw_span_t input;
                cfw_span_t output;
                size_t consumed = 0;
                int fin = 0;

                input.data = pending->data + *pos;
                input.size = pending->size - *pos;
                status = cfw_http_chunked_decode(&decoder, &input,
                                                 &output, &consumed,
                                                 &fin);
                if (status != CFW_OK) {
                    return CFW_ERROR_IO;
                }
                *pos += consumed;
                if (output.size != 0) {
                    status = cfw_client_body_append(response, output.data,
                                                    output.size, max_body);
                    if (status != CFW_OK) {
                        return status;
                    }
                }
                if (fin) {
                    finished = 1;
                }
                continue;
            }
            if (*input_closed) {
                return CFW_ERROR_IO;
            }
            status = cfw_client_fill(client, pending, pos, input_closed);
            if (status != CFW_OK) {
                return status;
            }
        }
        return CFW_OK;
    }

    case CFW_HTTP_BODY_UNTIL_CLOSE:
    default:
        for (;;) {
            size_t avail = pending->size - *pos;
            if (avail != 0) {
                status = cfw_client_body_append(response,
                                                pending->data + *pos,
                                                avail, max_body);
                if (status != CFW_OK) {
                    return status;
                }
                *pos = pending->size;
            }
            if (*input_closed) {
                return CFW_OK;
            }
            status = cfw_client_fill(client, pending, pos, input_closed);
            if (status != CFW_OK) {
                return status;
            }
        }
    }
}

/* Nonblocking connect with deadline: waits for writability then
 * checks SO_ERROR to distinguish success from refusal. */
static cfw_status_t cfw_client_connect(cfw_client_t *client,
                                       const cfw_addr_t *addr)
{
    cfw_status_t status;

    status = cfw_net_socket(cfw_addr_is_ipv6(addr) ? AF_INET6 : AF_INET,
                            SOCK_STREAM, 0, &client->fd);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_pollset_add(client->pollset,
                             (intptr_t)cfw_fd_get(&client->fd),
                             CFW_POLL_IN | CFW_POLL_OUT, NULL);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_net_connect(&client->fd, addr);
    if (status == CFW_OK) {
        return CFW_OK;
    }
    if (status != CFW_ERROR_WOULDBLOCK) {
        return status;
    }
    for (;;) {
        int sock_err = 0;
        status = cfw_client_wait(client, CFW_POLL_OUT);
        if (status != CFW_OK) {
            return status;
        }
        status = cfw_net_so_error(&client->fd, &sock_err);
        if (status != CFW_OK) {
            return status;
        }
        if (sock_err == 0) {
            return CFW_OK;
        }
        return CFW_ERROR_IO;
    }
}

/* TLS handshake driven by want_read/want_write hints and the
 * deadline. */
static cfw_status_t cfw_client_handshake(cfw_client_t *client)
{
    cfw_status_t status;

    for (;;) {
        status = cfw_tls_handshake(client->tls);
        if (status == CFW_OK) {
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        status = cfw_client_wait(client,
                                 cfw_client_tls_events(client, 0));
        if (status != CFW_OK) {
            return status;
        }
    }
}

cfw_status_t cfw_http_client_request(
    const cfw_http_client_request_t *request,
    cfw_http_client_response_t *response)
{
    cfw_client_t client;
    cfw_pollset_t *pollset = NULL;
    cfw_tls_conn_t *tls = NULL;
    cfw_buffer_t pending;
    cfw_buffer_t head_buf;
    memset(&pending, 0, sizeof(pending));
    memset(&head_buf, 0, sizeof(head_buf));
    cfw_http_head_reader_t reader;
    cfw_addr_t addr;
    cfw_addr_family_t families[2];
    size_t family_i;
    size_t pos = 0;
    int input_closed = 0;
    int head_reader_ready = 0;
    int pending_ready = 0;
    cfw_status_t status = CFW_ERROR_INTERNAL;
    cfw_strview_t target_view;
    cfw_strview_t head_view;
    cfw_http_response_head_t resp_head;
    cfw_http_message_framing_t framing;
    cfw_http_client_response_t *out = response;
    cfw_http_client_response_t discard;
    uint64_t max_body;
    size_t max_head;
    cfw_duration_t timeout;

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
    {
        size_t i;
        for (i = 0; i < request->header_count; i++) {
            if (request->headers[i].name.data == NULL ||
                cfw_client_name_is_forbidden(&request->headers[i].name)) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    if (out == NULL) {
        memset(&discard, 0, sizeof(discard));
        out = &discard;
    } else {
        memset(out, 0, sizeof(*out));
    }

    memset(&client, 0, sizeof(client));
    cfw_fd_init(&client.fd);
    client.has_deadline = 0;

    timeout = request->timeout;
    if (timeout.nanoseconds == 0) {
        status = cfw_duration_from_millis(CFW_HTTP_CLIENT_DEFAULT_TIMEOUT_MS,
                                          &timeout);
        if (status != CFW_OK) {
            goto done;
        }
    }
    status = cfw_time_now(&client.deadline);
    if (status != CFW_OK) {
        goto done;
    }
    {
        cfw_timepoint_t deadline;
        status = cfw_timepoint_add(&client.deadline, &timeout, &deadline);
        if (status != CFW_OK) {
            goto done;
        }
        client.deadline = deadline;
        client.has_deadline = 1;
    }

    max_head = request->max_head_size != 0
                   ? request->max_head_size
                   : CFW_HTTP_CLIENT_DEFAULT_MAX_HEAD;
    max_body = request->max_body_size != 0
                   ? request->max_body_size
                   : CFW_HTTP_CLIENT_DEFAULT_MAX_BODY;

    status = cfw_pollset_create(&pollset);
    if (status != CFW_OK) {
        goto done;
    }
    client.pollset = pollset;

    /* Resolve and connect: try IPv4 then IPv6 so dual-stack and
     * v6-only hosts both work. Each attempt shares the deadline. */
    families[0] = CFW_ADDR_FAMILY_IPV4;
    families[1] = CFW_ADDR_FAMILY_IPV6;
    status = CFW_ERROR_IO;
    for (family_i = 0; family_i < 2; family_i++) {
        cfw_status_t attempt;
        attempt = cfw_addr_resolve(&addr, request->host, request->port,
                                   families[family_i]);
        if (attempt != CFW_OK) {
            continue;
        }
        attempt = cfw_client_connect(&client, &addr);
        if (attempt == CFW_OK) {
            status = CFW_OK;
            break;
        }
        status = attempt;
        if (attempt == CFW_ERROR_TIMEOUT ||
            attempt == CFW_ERROR_UNAVAILABLE) {
            break;
        }
        /* The fd number may be reused by the next family's socket;
         * drop its pollset registration before closing. */
        (void)cfw_pollset_remove(
            pollset, (intptr_t)cfw_fd_get(&client.fd));
        cfw_fd_close(&client.fd);
        cfw_fd_init(&client.fd);
    }
    if (status != CFW_OK) {
        goto done;
    }

    /* Optional TLS layer over the connected socket. */
    if (request->tls_ctx != NULL) {
        status = cfw_tls_conn_create(request->tls_ctx,
                                     cfw_fd_get(&client.fd), &tls);
        if (status != CFW_OK) {
            goto done;
        }
        client.tls = tls;
        status = cfw_tls_conn_set_server_name(tls, request->host);
        if (status != CFW_OK) {
            goto done;
        }
        status = cfw_client_handshake(&client);
        if (status != CFW_OK) {
            goto done;
        }
    }

    /* Build the request head: managed headers first, then caller
     * headers, then framing headers last. */
    {
        cfw_buffer_t authority;
        cfw_http_header_t *headers_all = NULL;
        size_t total_headers = request->header_count + 4;
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

        memset(&authority, 0, sizeof(authority));
        bracket_v6 = strchr(request->host, ':') != NULL &&
                     request->host[0] != '[';
        status = cfw_buffer_append(&authority, request->host,
                                   strlen(request->host));
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
                                (unsigned)request->port);
            if (plen <= 0) {
                status = CFW_ERROR_INTERNAL;
            } else {
                status = cfw_buffer_append(&authority, port_text,
                                           (size_t)plen);
            }
        }
        if (status != CFW_OK) {
            cfw_buffer_free(&authority);
            goto done;
        }

        headers_all = (cfw_http_header_t *)malloc(
            total_headers * sizeof(*headers_all));
        if (headers_all == NULL) {
            cfw_buffer_free(&authority);
            status = CFW_ERROR_NOMEM;
            goto done;
        }

        host_value.data = (const char *)authority.data;
        host_value.size = authority.size;
        {
            int ua_len = snprintf(ua_text, sizeof(ua_text), "cfw/%s",
                                  cfw_get_version_string());
            int cl_len = snprintf(cl_text, sizeof(cl_text), "%llu",
                                  (unsigned long long)request->body_size);
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
            cfw_strview_from_cstr(&conn_value, "close");
            headers_all[0].name = hn;
            headers_all[0].value = host_value;
            headers_all[1].name = un;
            headers_all[1].value = ua_value;
            for (i = 0; i < request->header_count; i++) {
                headers_all[2 + i] = request->headers[i];
            }
            headers_all[2 + request->header_count].name = cn;
            headers_all[2 + request->header_count].value = cl_value;
            headers_all[3 + request->header_count].name = xn;
            headers_all[3 + request->header_count].value = conn_value;
        }

        /* Exact size for the complete head, then one-shot format. */
        target_view.data = request->target;
        target_view.size = strlen(request->target);
        need = 0;
        {
            const char *mname = cfw_http_method_name(request->method);
            /* "M target HTTP/1.1\r\n" + "\r\n" terminator. */
            size_t line_len = strlen(mname) + 1 + target_view.size + 1 +
                              8 + 2;
            need = line_len + 2;
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
        }
        status = cfw_buffer_reserve(&head_buf, need);
        if (status != CFW_OK) {
            goto head_fail;
        }
        {
            cfw_http_version_t version;
            version.major = 1;
            version.minor = 1;
            status = cfw_http_request_head_format(
                request->method, &target_view, &version, headers_all,
                total_headers, (char *)head_buf.data, head_buf.capacity,
                &off);
        }
        if (status != CFW_OK) {
            goto head_fail;
        }
        head_buf.size = off;

    head_fail:
        free(headers_all);
        cfw_buffer_free(&authority);
        if (status != CFW_OK) {
            goto done;
        }
    }

    /* Send request head, then body. */
    status = cfw_client_write_all(&client, head_buf.data, head_buf.size);
    if (status != CFW_OK) {
        goto done;
    }
    if (request->body_size != 0) {
        status = cfw_client_write_all(&client, request->body,
                                      request->body_size);
        if (status != CFW_OK) {
            goto done;
        }
    }

    /* Read response. */
    status = cfw_buffer_init(&pending);
    if (status != CFW_OK) {
        goto done;
    }
    pending_ready = 1;
    status = cfw_http_head_reader_init(&reader, max_head);
    if (status != CFW_OK) {
        goto done;
    }
    head_reader_ready = 1;
    status = cfw_buffer_init(&out->head);
    if (status != CFW_OK) {
        goto done;
    }
    status = cfw_buffer_init(&out->body);
    if (status != CFW_OK) {
        goto done;
    }

    status = cfw_client_read_head(&client, &reader, &pending, &pos,
                                  &input_closed, &head_view, &resp_head);
    if (status != CFW_OK) {
        goto done;
    }
    /* Persist the raw head so response_head() can re-parse it. */
    status = cfw_buffer_append(&out->head, head_view.data, head_view.size);
    if (status != CFW_OK) {
        goto done;
    }
    status = cfw_http_response_framing(&resp_head, request->method,
                                       &framing);
    if (status != CFW_OK) {
        status = CFW_ERROR_IO;
        goto done;
    }
    status = cfw_client_read_body(&client, out, &pending, &pos,
                                  &input_closed, &framing, max_body);
    if (status != CFW_OK) {
        goto done;
    }
    status = CFW_OK;

done:
    if (head_reader_ready) {
        cfw_http_head_reader_free(&reader);
    }
    if (pending_ready) {
        cfw_buffer_free(&pending);
    }
    cfw_buffer_free(&head_buf);
    if (tls != NULL) {
        cfw_tls_conn_destroy(tls);
    }
    cfw_fd_close(&client.fd);
    cfw_pollset_destroy(pollset);
    if (out == &discard) {
        cfw_http_client_response_free(&discard);
    } else if (status != CFW_OK && response != NULL) {
        /* Failure: discard partial results but keep buffers valid. */
        cfw_buffer_clear(&response->head);
        cfw_buffer_clear(&response->body);
    }
    return status;
}

#endif /* POSIX */
