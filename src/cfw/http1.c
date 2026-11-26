/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/http1.h>

#include <string.h>

static cfw_strview_t cfw_http1_pending_span(const cfw_http1_server_t *server)
{
    cfw_strview_t view;
    view.data = (const char *)server->pending.data + server->pending_pos;
    view.size = server->pending.size - server->pending_pos;
    return view;
}

static void cfw_http1_pending_advance(cfw_http1_server_t *server, size_t n)
{
    server->pending_pos += n;
}

static cfw_status_t cfw_http1_pending_reserve(cfw_http1_server_t *server,
                                              size_t extra)
{
    size_t logical = server->pending.size - server->pending_pos;
    cfw_status_t status;

    if (extra > server->max_pending_size ||
        logical > server->max_pending_size - extra) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (server->pending_pos != 0) {
        memmove(server->pending.data, server->pending.data + server->pending_pos,
                logical);
        server->pending.size = logical;
        server->pending_pos = 0;
    }
    status = cfw_buffer_reserve(&server->pending,
                                server->pending.size + extra);
    if (status != CFW_OK) {
        return status;
    }
    return CFW_OK;
}

/* Returns nonzero when the Connection header carries `token` as a
 * comma-separated element (case-insensitive, OWS trimmed). */
static int cfw_http1_connection_has(const cfw_http_request_head_t *head,
                                    const char *token)
{
    cfw_strview_t block = head->headers;
    cfw_strview_t connection_name;
    cfw_strview_t wanted;
    cfw_http_header_t header;
    size_t cursor = 0;
    int done;
    cfw_status_t status;

    cfw_strview_from_cstr(&connection_name, "connection");
    cfw_strview_from_cstr(&wanted, token);

    for (;;) {
        status = cfw_http_headers_next(&block, &cursor, &header, &done);
        if (status != CFW_OK || done) {
            return 0;
        }
        {
            int equal;
            if (cfw_strview_equal_nocase(&header.name, &connection_name,
                                         &equal) != CFW_OK || !equal) {
                continue;
            }
        }
        /* Scan comma-separated tokens in the value. */
        {
            size_t i = 0;
            while (i < header.value.size) {
                size_t start = i;
                size_t end;
                cfw_strview_t element;
                int match;
                while (i < header.value.size &&
                       header.value.data[i] != ',') {
                    i++;
                }
                end = i;
                if (i < header.value.size) {
                    i++; /* skip comma */
                }
                /* Trim OWS. */
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
                element.data = header.value.data + start;
                element.size = end - start;
                if (cfw_strview_equal_nocase(&element, &wanted, &match) ==
                        CFW_OK &&
                    match) {
                    return 1;
                }
            }
        }
    }
}

static void cfw_http1_compute_keep_alive(cfw_http1_server_t *server)
{
    const cfw_http_request_line_t *line = &server->request_head.request_line;
    int persistent_default;

    if (line->version.major > 1 ||
        (line->version.major == 1 && line->version.minor >= 1)) {
        persistent_default = 1;
    } else {
        persistent_default = 0;
    }

    if (cfw_http1_connection_has(&server->request_head, "close")) {
        server->keep_alive = 0;
    } else if (persistent_default) {
        server->keep_alive = 1;
    } else {
        server->keep_alive =
            cfw_http1_connection_has(&server->request_head, "keep-alive");
    }
}

cfw_status_t cfw_http1_server_init(cfw_http1_server_t *server,
                                   const cfw_http1_server_options_t *options)
{
    cfw_status_t status;

    if (server == NULL || options == NULL || options->max_head_size == 0 ||
        options->max_pending_size == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    memset(server, 0, sizeof(*server));

    status = cfw_http_head_reader_init(&server->head_reader,
                                       options->max_head_size);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_buffer_init(&server->pending);
    if (status != CFW_OK) {
        cfw_http_head_reader_free(&server->head_reader);
        return status;
    }
    status = cfw_buffer_init(&server->head_storage);
    if (status != CFW_OK) {
        cfw_buffer_free(&server->pending);
        cfw_http_head_reader_free(&server->head_reader);
        return status;
    }

    cfw_http_chunked_decoder_init(&server->chunked);
    server->state = CFW_HTTP1_STATE_HEAD;
    server->max_head_size = options->max_head_size;
    server->max_pending_size = options->max_pending_size;
    server->max_body_size = options->max_body_size;
    return CFW_OK;
}

void cfw_http1_server_free(cfw_http1_server_t *server)
{
    if (server == NULL) {
        return;
    }
    cfw_http_head_reader_free(&server->head_reader);
    cfw_buffer_free(&server->pending);
    cfw_buffer_free(&server->head_storage);
    memset(server, 0, sizeof(*server));
}

cfw_status_t cfw_http1_server_feed(cfw_http1_server_t *server,
                                   const cfw_span_t *input)
{
    cfw_status_t status;

    if (server == NULL || input == NULL ||
        (input->size != 0 && input->data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (server->failed || server->state == CFW_HTTP1_STATE_CLOSED) {
        return CFW_ERROR_WOULDBLOCK;
    }
    if (input->size == 0) {
        return CFW_OK;
    }

    status = cfw_http1_pending_reserve(server, input->size);
    if (status != CFW_OK) {
        server->failed = 1;
        return status;
    }
    status = cfw_buffer_append(&server->pending, input->data, input->size);
    if (status != CFW_OK) {
        server->failed = 1;
        return status;
    }
    return CFW_OK;
}

static cfw_status_t cfw_http1_fail(cfw_http1_server_t *server,
                                   cfw_http1_event_t *event,
                                   cfw_http1_error_kind_t kind)
{
    server->failed = 1;
    server->state = CFW_HTTP1_STATE_CLOSED;
    event->kind = CFW_HTTP1_EV_ERROR;
    event->error_kind = kind;
    return CFW_OK;
}

/* Attempts to complete the current request head. Returns CFW_OK and fills
 * *ready when a head was parsed into server->request_head, CFW_ERROR_
 * WOULDBLOCK when more input is needed, or a protocol error via
 * cfw_http1_fail. */
static cfw_status_t cfw_http1_try_head(cfw_http1_server_t *server,
                                       cfw_http1_event_t *event, int *ready)
{
    cfw_strview_t pending;
    cfw_strview_t head_view;
    cfw_span_t head_span;
    size_t consumed = 0;
    cfw_status_t status;

    *ready = 0;

    if (!cfw_http_head_reader_is_complete(&server->head_reader)) {
        pending = cfw_http1_pending_span(server);
        if (pending.size == 0) {
            return CFW_ERROR_WOULDBLOCK;
        }
        cfw_span_init(&head_span, pending.data, pending.size);
        status = cfw_http_head_reader_feed(&server->head_reader, &head_span,
                                           &consumed);
        if (status != CFW_OK) {
            return cfw_http1_fail(server, event, CFW_HTTP1_ERR_PROTOCOL);
        }
        cfw_http1_pending_advance(server, consumed);
        if (!cfw_http_head_reader_is_complete(&server->head_reader)) {
            return CFW_ERROR_WOULDBLOCK;
        }
    }

    /* Move the head bytes into stable storage and parse. */
    status = cfw_http_head_reader_head(&server->head_reader, &head_view);
    if (status != CFW_OK) {
        return cfw_http1_fail(server, event, CFW_HTTP1_ERR_PROTOCOL);
    }
    cfw_buffer_clear(&server->head_storage);
    status = cfw_buffer_append(&server->head_storage, head_view.data,
                               head_view.size);
    if (status != CFW_OK) {
        return cfw_http1_fail(server, event, CFW_HTTP1_ERR_PROTOCOL);
    }
    {
        cfw_strview_t stored;
        stored.data = (const char *)server->head_storage.data;
        stored.size = server->head_storage.size;
        status = cfw_http_request_head_parse(&stored, &server->request_head);
        if (status != CFW_OK) {
            return cfw_http1_fail(server, event, CFW_HTTP1_ERR_PROTOCOL);
        }
    }

    status = cfw_http_request_framing(&server->request_head,
                                      &server->framing);
    if (status != CFW_OK) {
        return cfw_http1_fail(server, event, CFW_HTTP1_ERR_PROTOCOL);
    }
    if (server->framing.framing == CFW_HTTP_BODY_CONTENT_LENGTH &&
        server->max_body_size != 0 &&
        server->framing.content_length > server->max_body_size) {
        return cfw_http1_fail(server, event, CFW_HTTP1_ERR_BODY_TOO_LARGE);
    }

    cfw_http1_compute_keep_alive(server);
    server->body_remaining =
        server->framing.framing == CFW_HTTP_BODY_CONTENT_LENGTH
            ? server->framing.content_length
            : 0;
    server->body_seen = 0;
    cfw_http_chunked_decoder_init(&server->chunked);
    cfw_http_head_reader_reset(&server->head_reader);

    if (server->framing.framing == CFW_HTTP_BODY_CHUNKED) {
        server->state = CFW_HTTP1_STATE_BODY_CHUNKED;
    } else if (server->framing.framing == CFW_HTTP_BODY_CONTENT_LENGTH) {
        server->state = CFW_HTTP1_STATE_BODY_CL;
    } else {
        /* No body: emit MESSAGE_DONE on the next call. */
        server->state = CFW_HTTP1_STATE_BODY_CL;
        server->body_remaining = 0;
    }
    *ready = 1;
    return CFW_OK;
}

static void cfw_http1_message_done(cfw_http1_server_t *server,
                                   cfw_http1_event_t *event)
{
    event->kind = CFW_HTTP1_EV_MESSAGE_DONE;
    if (!server->keep_alive) {
        server->state = CFW_HTTP1_STATE_CLOSED;
    } else {
        server->state = CFW_HTTP1_STATE_HEAD;
    }
}

cfw_status_t cfw_http1_server_next_event(cfw_http1_server_t *server,
                                         cfw_http1_event_t *event)
{
    cfw_status_t status;
    int ready;

    if (server == NULL || event == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(event, 0, sizeof(*event));

    if (server->failed || server->state == CFW_HTTP1_STATE_CLOSED) {
        return CFW_ERROR_WOULDBLOCK;
    }

    for (;;) {
        switch (server->state) {
        case CFW_HTTP1_STATE_HEAD: {
            status = cfw_http1_try_head(server, event, &ready);
            if (status == CFW_ERROR_WOULDBLOCK) {
                return CFW_ERROR_WOULDBLOCK;
            }
            if (status != CFW_OK) {
                return status;
            }
            if (event->kind == CFW_HTTP1_EV_ERROR) {
                return CFW_OK;
            }
            if (!ready) {
                return CFW_ERROR_WOULDBLOCK;
            }
            event->kind = CFW_HTTP1_EV_HEAD;
            event->head = server->request_head;
            return CFW_OK;
        }
        case CFW_HTTP1_STATE_BODY_CL:
            if (server->body_remaining == 0) {
                cfw_http1_message_done(server, event);
                return CFW_OK;
            }
            {
                cfw_strview_t pending = cfw_http1_pending_span(server);
                size_t n;
                if (pending.size == 0) {
                    return CFW_ERROR_WOULDBLOCK;
                }
                n = pending.size;
                if ((uint64_t)n > server->body_remaining) {
                    n = (size_t)server->body_remaining;
                }
                event->kind = CFW_HTTP1_EV_BODY;
                event->body.data = (const uint8_t *)pending.data;
                event->body.size = n;
                server->body_remaining -= (uint64_t)n;
                server->body_seen += (uint64_t)n;
                cfw_http1_pending_advance(server, n);
                return CFW_OK;
            }
        case CFW_HTTP1_STATE_BODY_CHUNKED: {
            cfw_strview_t pending = cfw_http1_pending_span(server);
            cfw_span_t input;
            cfw_span_t output;
            size_t consumed = 0;
            int finished = 0;

            if (pending.size == 0) {
                /* The decoder may have finished on a previous call that
                 * also produced the last body span. */
                if (server->chunked.done) {
                    cfw_http1_message_done(server, event);
                    return CFW_OK;
                }
                return CFW_ERROR_WOULDBLOCK;
            }
            cfw_span_init(&input, pending.data, pending.size);
            status = cfw_http_chunked_decode(&server->chunked, &input,
                                             &output, &consumed, &finished);
            if (status != CFW_OK) {
                return cfw_http1_fail(server, event, CFW_HTTP1_ERR_PROTOCOL);
            }
            cfw_http1_pending_advance(server, consumed);

            if (output.size != 0) {
                if (server->max_body_size != 0 &&
                    server->body_seen + output.size > server->max_body_size) {
                    return cfw_http1_fail(server, event,
                                          CFW_HTTP1_ERR_BODY_TOO_LARGE);
                }
                server->body_seen += output.size;
                event->kind = CFW_HTTP1_EV_BODY;
                event->body = output;
                return CFW_OK;
            }
            if (finished) {
                cfw_http1_message_done(server, event);
                return CFW_OK;
            }
            /* Consumed only framing bytes; loop for more output or input. */
            if (server->pending_pos >= server->pending.size) {
                return CFW_ERROR_WOULDBLOCK;
            }
            break;
        }
        default:
            return CFW_ERROR_WOULDBLOCK;
        }
    }
}
