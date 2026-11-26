/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_HTTP1_H
#define CFW_HTTP1_H

#include <cfw/http.h>
#include <cfw/span.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP/1.1 server-side message reader. Combines the head reader, framing
 * detection, and body decoders into a per-connection state machine with
 * keep-alive and HTTP pipelining support.
 *
 * Usage model (pull-based, single-threaded per connection):
 *
 *   cfw_http1_server_t server;
 *   cfw_http1_server_init(&server, &options);
 *   for (;;) {
 *       n = recv(...);
 *       cfw_http1_server_feed(&server, &span);
 *       for (;;) {
 *           cfw_http1_event_t event;
 *           st = cfw_http1_server_next_event(&server, &event);
 *           if (st == CFW_ERROR_WOULDBLOCK) break;   // need more input
 *           if (st != CFW_OK) { handle fatal error }
 *           switch (event.kind) { ... }
 *       }
 *   }
 *   cfw_http1_server_free(&server);
 *
 * All events must be drained (until WOULDBLOCK) before the next feed.
 * Event spans and head strviews point into server-owned storage and
 * remain valid until the next feed call or until the next HEAD event.
 */

typedef struct cfw_http1_server_options {
    /* Maximum request-head size in bytes; must be > 0. */
    size_t max_head_size;
    /* Maximum message-body size in bytes; 0 means unlimited. */
    uint64_t max_body_size;
    /* Maximum buffered-but-unprocessed input; must be > 0. Bounds memory
     * per connection against fast clients and pipelined floods. */
    size_t max_pending_size;
} cfw_http1_server_options_t;

typedef enum cfw_http1_event_kind {
    /* A complete, framing-valid request head is available in event.head. */
    CFW_HTTP1_EV_HEAD = 0,
    /* Decoded body bytes are available in event.body. May be partial;
     * MESSAGE_DONE marks the end of the body. */
    CFW_HTTP1_EV_BODY,
    /* The request message is fully consumed. On keep-alive connections the
     * next HEAD event (if any) follows for a pipelined request. */
    CFW_HTTP1_EV_MESSAGE_DONE,
    /* A protocol or resource-limit violation occurred; no further events
     * will be produced for this connection. event.error_kind classifies
     * it (400 vs 413). */
    CFW_HTTP1_EV_ERROR
} cfw_http1_event_kind_t;

typedef enum cfw_http1_error_kind {
    /* Malformed request, bad framing, oversized head, or connection
     * misuse: respond 400 Bad Request. */
    CFW_HTTP1_ERR_PROTOCOL = 0,
    /* Body exceeded max_body_size: respond 413 Payload Too Large. */
    CFW_HTTP1_ERR_BODY_TOO_LARGE
} cfw_http1_error_kind_t;

typedef struct cfw_http1_event {
    cfw_http1_event_kind_t kind;
    cfw_http1_error_kind_t error_kind;
    cfw_http_request_head_t head;
    cfw_span_t body;
} cfw_http1_event_t;

/* Opaque state is kept in this struct for allocation-free use; callers
 * must not access fields directly. */
typedef struct cfw_http1_server {
    cfw_http_head_reader_t head_reader;
    cfw_http_chunked_decoder_t chunked;
    cfw_buffer_t pending;
    size_t pending_pos;
    cfw_buffer_t head_storage;
    cfw_http_request_head_t request_head;
    cfw_http_message_framing_t framing;
    uint64_t body_remaining;
    uint64_t body_seen;
    int state;
    int keep_alive;
    int failed;
    size_t max_head_size;
    size_t max_pending_size;
    uint64_t max_body_size;
} cfw_http1_server_t;

enum {
    CFW_HTTP1_STATE_HEAD = 0,
    CFW_HTTP1_STATE_BODY_CL,
    CFW_HTTP1_STATE_BODY_CHUNKED,
    CFW_HTTP1_STATE_CLOSED
};

CFW_API cfw_status_t cfw_http1_server_init(cfw_http1_server_t *server,
                                           const cfw_http1_server_options_t *options);
CFW_API void cfw_http1_server_free(cfw_http1_server_t *server);

/* Buffers transport input. Returns CFW_ERROR_INVALID_ARGUMENT if the
 * pending buffer would exceed max_pending_size, or CFW_ERROR_WOULDBLOCK
 * if the connection already failed or closed (input is ignored). */
CFW_API cfw_status_t cfw_http1_server_feed(cfw_http1_server_t *server,
                                           const cfw_span_t *input);

/* Produces the next event. Returns CFW_OK with event->kind set, or
 * CFW_ERROR_WOULDBLOCK when more input is needed (or the connection is
 * closed after MESSAGE_DONE on a non-keep-alive request). */
CFW_API cfw_status_t cfw_http1_server_next_event(cfw_http1_server_t *server,
                                                 cfw_http1_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* CFW_HTTP1_H */
