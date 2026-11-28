/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/ws_client.h>

#include "private/http_fields.h"
#include "private/platform_detect.h"
#include "private/ws_proto.h"

#include <cfw/ws_client.h>

#include <cfw/buffer.h>
#include <cfw/codec.h>
#include <cfw/fd.h>
#include <cfw/http.h>
#include <cfw/net.h>
#include <cfw/pollset.h>
#include <cfw/random.h>
#include <cfw/strview.h>
#include <cfw/tls.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CFW_PLATFORM_WINDOWS
    #include <winsock2.h>
#else
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

#define CFW_WSC_DEFAULT_TIMEOUT_MS 10000
#define CFW_WSC_DEFAULT_MAX_MESSAGE (1024u * 1024u)
#define CFW_WSC_FRAME_PAYLOAD 16384u
#define CFW_WSC_READ_BUF 16384u
#define CFW_WSC_MAX_HEAD 16384u
#define CFW_WSC_MAX_CTRL 125u
#define CFW_WSC_HDR_CAP 14u
#define CFW_WSC_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

struct cfw_ws_client {
    cfw_fd_t fd;
    cfw_pollset_t *pollset;
    cfw_tls_conn_t *tls;
    cfw_duration_t io_timeout;
    cfw_timepoint_t deadline;
    int has_deadline;
    cfw_ws_client_message_fn on_message;
    void *user;
    size_t max_message_size;

    /* Incremental frame parser (server->client frames are unmasked). */
    cfw_buffer_t assembly;
    int have_header;
    size_t hdr_len;
    uint8_t hdr[CFW_WSC_HDR_CAP];
    uint64_t frame_remaining;
    int frame_fin;
    cfw_ws_opcode_t frame_op;
    int frame_to_asm;
    uint8_t ctrl_buf[CFW_WSC_MAX_CTRL];
    size_t ctrl_len;
    int in_message;
    cfw_ws_opcode_t msg_op;

    int closed;     /* terminal state: transport dead or ws closed */
    int close_sent; /* we sent a close frame */
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Header names the client manages itself; caller-supplied headers
 * using them are rejected. */
static const char *const cfw_wsc_forbidden_headers[] = {
    "host",          "connection",   "upgrade",
    "sec-websocket-key", "sec-websocket-version",
    "sec-websocket-accept", "sec-websocket-extensions",
    "sec-websocket-protocol", "transfer-encoding",
    "keep-alive",    "te",           "trailer",
    "proxy-authenticate", "proxy-authorization",
};

static int cfw_wsc_name_forbidden(const cfw_strview_t *name)
{
    size_t i;
    for (i = 0; i < sizeof(cfw_wsc_forbidden_headers) /
                        sizeof(cfw_wsc_forbidden_headers[0]);
         i++) {
        cfw_strview_t forbidden;
        int equal = 0;
        if (cfw_strview_from_cstr(&forbidden,
                                  cfw_wsc_forbidden_headers[i]) !=
                CFW_OK ||
            cfw_strview_equal_nocase(name, &forbidden, &equal) !=
                CFW_OK) {
            return 0;
        }
        if (equal) {
            return 1;
        }
    }
    return 0;
}

static int cfw_wsc_value_is(const cfw_strview_t *value,
                            const char *text)
{
    cfw_strview_t want;
    int equal = 0;
    if (cfw_strview_from_cstr(&want, text) != CFW_OK ||
        cfw_strview_equal_nocase(value, &want, &equal) != CFW_OK) {
        return 0;
    }
    return equal;
}

/* True when the comma-separated header value contains `token`. */
static int cfw_wsc_value_has_token(const cfw_strview_t *value,
                                   const char *token)
{
    cfw_strview_t t;
    size_t pos = 0;
    if (cfw_strview_from_cstr(&t, token) != CFW_OK) {
        return 0;
    }
    while (pos <= value->size) {
        cfw_strview_t part;
        size_t start = pos;
        int equal = 0;
        while (pos < value->size && value->data[pos] != ',') {
            pos++;
        }
        part.data = value->data + start;
        part.size = pos - start;
        /* Trim OWS around the token. */
        while (part.size != 0 &&
               (part.data[0] == ' ' || part.data[0] == '\t')) {
            part.data++;
            part.size--;
        }
        while (part.size != 0 &&
               (part.data[part.size - 1] == ' ' ||
                part.data[part.size - 1] == '\t')) {
            part.size--;
        }
        if (cfw_strview_equal_nocase(&part, &t, &equal) == CFW_OK &&
            equal) {
            return 1;
        }
        if (pos == value->size) {
            break;
        }
        pos++;
    }
    return 0;
}

/* Sets the per-operation deadline: has_deadline=0 means wait
 * forever (pump with NULL timeout). */
static cfw_status_t cfw_wsc_set_deadline(cfw_ws_client_t *ws,
                                         const cfw_duration_t *timeout)
{
    if (timeout == NULL) {
        ws->has_deadline = 0;
        return CFW_OK;
    }
    ws->has_deadline = 1;
    return cfw_deadline_from_now(&ws->deadline, *timeout);
}

/* Waits until the socket is readable/writable per `events`, bounded
 * by the current op deadline (infinite when has_deadline == 0). */
static cfw_status_t cfw_wsc_wait(cfw_ws_client_t *ws, uint32_t events)
{
    cfw_poll_event_t ready[1];
    size_t count = 0;
    cfw_status_t status;

    status = cfw_pollset_modify(ws->pollset,
                                (intptr_t)cfw_fd_get(&ws->fd), events,
                                NULL);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_pollset_wait(ws->pollset,
                              ws->has_deadline ? &ws->deadline : NULL,
                              ready, 1, &count);
    if (status == CFW_ERROR_TIMEOUT) {
        return CFW_ERROR_TIMEOUT;
    }
    if (status != CFW_OK) {
        return status;
    }
    /* Event flags (ERR/HUP) are not fatal by themselves: the follow-up
     * I/O attempt reports the real outcome. */
    (void)count;
    (void)ready;
    return CFW_OK;
}

/* Direction mask for a WOULDBLOCK retry. */
static uint32_t cfw_wsc_tls_events(const cfw_ws_client_t *ws,
                                   int for_write)
{
    uint32_t events = 0;
    if (ws->tls != NULL) {
        if (cfw_tls_conn_want_read(ws->tls)) {
            events |= CFW_POLL_IN;
        }
        if (cfw_tls_conn_want_write(ws->tls)) {
            events |= CFW_POLL_OUT;
        }
    }
    if (events == 0) {
        events = for_write ? CFW_POLL_OUT : CFW_POLL_IN;
    }
    return events;
}

static cfw_status_t cfw_wsc_read(cfw_ws_client_t *ws, void *buffer,
                                 size_t size, size_t *done, int *eof)
{
    cfw_status_t status;

    *eof = 0;
    for (;;) {
        if (ws->tls != NULL) {
            status = cfw_tls_read(ws->tls, buffer, size, done);
        } else {
            status = cfw_net_recv(&ws->fd, buffer, size, done);
        }
        if (status == CFW_OK) {
            *eof = (*done == 0);
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        /* Decrypted-but-unread TLS bytes need no socket wait. */
        if (ws->tls != NULL && cfw_tls_pending(ws->tls) != 0) {
            return CFW_ERROR_WOULDBLOCK;
        }
        status = cfw_wsc_wait(ws, cfw_wsc_tls_events(ws, 0));
        if (status != CFW_OK) {
            return status;
        }
    }
}

static cfw_status_t cfw_wsc_write(cfw_ws_client_t *ws, const void *data,
                                  size_t size, size_t *done)
{
    cfw_status_t status;

    for (;;) {
        if (ws->tls != NULL) {
            status = cfw_tls_write(ws->tls, data, size, done);
        } else {
            status = cfw_net_send(&ws->fd, data, size, done);
        }
        if (status == CFW_OK) {
            if (*done == 0) {
                return CFW_ERROR_IO;
            }
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        status = cfw_wsc_wait(ws, cfw_wsc_tls_events(ws, 1));
        if (status != CFW_OK) {
            return status;
        }
    }
}

/* Writes all of data, waiting on the op deadline between partial
 * writes. */
static cfw_status_t cfw_wsc_write_all(cfw_ws_client_t *ws,
                                      const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t off = 0;

    while (off < size) {
        size_t done = 0;
        cfw_status_t status =
            cfw_wsc_write(ws, p + off, size - off, &done);
        if (status != CFW_OK) {
            return status;
        }
        off += done;
    }
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* Frame codec (client role: inbound unmasked, outbound masked)        */
/* ------------------------------------------------------------------ */

/* Terminates the connection after attempting a close frame with the
 * given status. Best-effort: write failures are ignored because the
 * connection is already doomed. */
static void cfw_wsc_fail(cfw_ws_client_t *ws, uint16_t status);

static cfw_status_t cfw_wsc_send_frame(cfw_ws_client_t *ws, int fin,
                                       cfw_ws_opcode_t opcode,
                                       const void *data, size_t size)
{
    uint8_t hdr[14];
    uint8_t mask[4];
    uint8_t buf[CFW_WSC_FRAME_PAYLOAD];
    size_t hn = 0;
    size_t off = 0;
    size_t mask_off = 0;
    cfw_status_t st;

    if (ws->closed) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size > INT64_MAX) {
        return CFW_ERROR_OVERFLOW;
    }
    st = cfw_random_bytes(mask, sizeof(mask));
    if (st != CFW_OK) {
        return st;
    }

    hdr[hn++] = (uint8_t)((fin ? 0x80 : 0) | (uint8_t)opcode);
    if (size < 126) {
        hdr[hn++] = (uint8_t)(0x80 | size);
    } else if (size <= 0xffff) {
        hdr[hn++] = (uint8_t)(0x80 | 126);
        hdr[hn++] = (uint8_t)(size >> 8);
        hdr[hn++] = (uint8_t)size;
    } else {
        int i;
        hdr[hn++] = (uint8_t)(0x80 | 127);
        for (i = 0; i < 8; i++) {
            hdr[hn + i] = (uint8_t)((uint64_t)size >> ((7 - i) * 8));
        }
        hn += 8;
    }
    memcpy(hdr + hn, mask, 4);
    hn += 4;

    st = cfw_wsc_write_all(ws, hdr, hn);
    if (st != CFW_OK) {
        return st;
    }
    while (off < size) {
        size_t n = size - off;
        size_t i;
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        for (i = 0; i < n; i++) {
            buf[i] = ((const uint8_t *)data)[off + i] ^
                     mask[(mask_off + i) & 3];
        }
        mask_off = (mask_off + n) & 3;
        st = cfw_wsc_write_all(ws, buf, n);
        if (st != CFW_OK) {
            return st;
        }
        off += n;
    }
    return CFW_OK;
}

static void cfw_wsc_fail(cfw_ws_client_t *ws, uint16_t status)
{
    uint8_t payload[2];
    size_t plen = 0;

    if (ws->closed) {
        return;
    }
    if (status != 0) {
        payload[0] = (uint8_t)(status >> 8);
        payload[1] = (uint8_t)status;
        plen = 2;
    }
    /* Send BEFORE marking closed: cfw_wsc_send_frame rejects closed
     * connections. Write failures are ignored — the connection is
     * already doomed. */
    (void)cfw_wsc_send_frame(ws, 1, CFW_WS_OPCODE_CLOSE, payload, plen);
    ws->closed = 1;
}

/* Sends a fragmented data message (or single control frame). */
static cfw_status_t cfw_wsc_send_data(cfw_ws_client_t *ws,
                                      cfw_ws_opcode_t opcode,
                                      const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t off = 0;
    cfw_status_t st;

    if (ws->closed) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if ((opcode == CFW_WS_OPCODE_TEXT ||
         opcode == CFW_WS_OPCODE_BINARY) &&
        opcode != CFW_WS_OPCODE_CONTINUATION) {
        if (size == 0 || size <= CFW_WSC_FRAME_PAYLOAD) {
            return cfw_wsc_send_frame(ws, 1, opcode, data, size);
        }
        st = cfw_wsc_send_frame(ws, 0, opcode, bytes,
                                CFW_WSC_FRAME_PAYLOAD);
        if (st != CFW_OK) {
            return st;
        }
        off = CFW_WSC_FRAME_PAYLOAD;
        while (off < size) {
            size_t n = size - off;
            int fin;
            if (n > CFW_WSC_FRAME_PAYLOAD) {
                n = CFW_WSC_FRAME_PAYLOAD;
                fin = 0;
            } else {
                fin = 1;
            }
            st = cfw_wsc_send_frame(ws, fin, CFW_WS_OPCODE_CONTINUATION,
                                    bytes + off, n);
            if (st != CFW_OK) {
                return st;
            }
            off += n;
        }
        return CFW_OK;
    }
    return cfw_wsc_send_frame(ws, 1, opcode, data, size);
}

/* ------------------------------------------------------------------ */
/* Incremental parser (client role)                                    */
/* ------------------------------------------------------------------ */

static void cfw_wsc_parse_header(cfw_ws_client_t *ws)
{
    const uint8_t *h = ws->hdr;
    uint8_t b0 = h[0];
    uint8_t b1 = h[1];
    unsigned opcode = b0 & 0x0f;
    uint64_t len;
    size_t ext = 0;
    size_t pos = 2;

    ws->frame_fin = (b0 & 0x80) != 0;
    ws->frame_op = (cfw_ws_opcode_t)opcode;

    /* RSV bits must be zero (no extensions negotiated). */
    if ((b0 & 0x70) != 0) {
        cfw_wsc_fail(ws, 1002);
        return;
    }
    /* Server->client frames must NOT be masked (RFC 6455 5.1). */
    if ((b1 & 0x80) != 0) {
        cfw_wsc_fail(ws, 1002);
        return;
    }
    switch (opcode) {
    case CFW_WS_OPCODE_CONTINUATION:
        if (!ws->in_message) {
            cfw_wsc_fail(ws, 1002);
            return;
        }
        break;
    case CFW_WS_OPCODE_TEXT:
    case CFW_WS_OPCODE_BINARY:
        if (ws->in_message) {
            cfw_wsc_fail(ws, 1002);
            return;
        }
        break;
    case CFW_WS_OPCODE_CLOSE:
    case CFW_WS_OPCODE_PING:
    case CFW_WS_OPCODE_PONG:
        if (!ws->frame_fin) {
            cfw_wsc_fail(ws, 1002);
            return;
        }
        break;
    default:
        cfw_wsc_fail(ws, 1002);
        return;
    }

    {
        uint8_t len7 = b1 & 0x7f;
        if (len7 == 126) {
            ext = 2;
        } else if (len7 == 127) {
            ext = 8;
        }
        len = len7;
    }
    if (ext == 2) {
        len = ((uint64_t)h[pos] << 8) | h[pos + 1];
        pos += 2;
    } else if (ext == 8) {
        int i;
        if (h[pos] & 0x80) {
            cfw_wsc_fail(ws, 1002); /* length MSB must be 0 */
            return;
        }
        len = 0;
        for (i = 0; i < 8; i++) {
            len = (len << 8) | h[pos + i];
        }
        pos += 8;
    }
    (void)pos;

    /* Control frames are limited to 125 bytes of payload. */
    if (opcode >= CFW_WS_OPCODE_CLOSE && len > CFW_WSC_MAX_CTRL) {
        cfw_wsc_fail(ws, 1002);
        return;
    }
    /* Data frames must fit the message bound including prior
     * fragments. */
    if (opcode <= CFW_WS_OPCODE_BINARY) {
        if (len > (uint64_t)ws->max_message_size ||
            ws->assembly.size >
                ws->max_message_size - (size_t)len) {
            cfw_wsc_fail(ws, 1009);
            return;
        }
    }
    ws->frame_remaining = len;
    ws->ctrl_len = 0;
    ws->frame_to_asm = opcode <= CFW_WS_OPCODE_BINARY;
}

/* Handles a fully-received control payload (close/ping/pong). */
static void cfw_wsc_handle_ctrl(cfw_ws_client_t *ws)
{
    const uint8_t *p = ws->ctrl_buf;
    size_t n = ws->ctrl_len;

    switch (ws->frame_op) {
    case CFW_WS_OPCODE_CLOSE: {
        uint16_t status = 0;
        int valid = 1;
        if (n == 1) {
            valid = 0;
        } else if (n >= 2) {
            status = (uint16_t)((p[0] << 8) | p[1]);
            if (!cfw_ws_close_status_ok(status)) {
                valid = 0;
            } else if (n > 2 && !cfw_ws_utf8_valid(p + 2, n - 2)) {
                cfw_wsc_fail(ws, 1007);
                return;
            }
        }
        if (!valid) {
            cfw_wsc_fail(ws, 1002);
            return;
        }
        /* Echo the close payload unless we already sent one; the
         * connection is then done. */
        if (!ws->close_sent) {
            (void)cfw_wsc_send_frame(ws, 1, CFW_WS_OPCODE_CLOSE, p, n);
        }
        ws->closed = 1;
        return;
    }
    case CFW_WS_OPCODE_PING:
        (void)cfw_wsc_send_frame(ws, 1, CFW_WS_OPCODE_PONG, p, n);
        return;
    case CFW_WS_OPCODE_PONG:
    default:
        return;
    }
}

/* Delivers a completed data message to the application. */
static void cfw_wsc_deliver(cfw_ws_client_t *ws)
{
    cfw_ws_message_t msg;
    cfw_span_t payload;
    cfw_status_t st;

    memset(&msg, 0, sizeof(msg));
    cfw_span_init(&payload, ws->assembly.data, ws->assembly.size);
    msg.opcode = ws->msg_op;
    msg.payload = payload;

    if (msg.opcode == CFW_WS_OPCODE_TEXT &&
        !cfw_ws_utf8_valid(payload.data, payload.size)) {
        cfw_wsc_fail(ws, 1007);
        return;
    }
    ws->assembly.size = 0;
    ws->in_message = 0;
    if (ws->on_message != NULL) {
        st = ws->on_message(ws, &msg, ws->user);
        if (st != CFW_OK) {
            cfw_wsc_fail(ws, 1011);
        }
    }
}

/* Called when a frame's payload has been fully consumed. */
static void cfw_wsc_frame_done(cfw_ws_client_t *ws)
{
    if (!ws->frame_to_asm) {
        cfw_wsc_handle_ctrl(ws);
        return;
    }
    if (ws->frame_op == CFW_WS_OPCODE_CONTINUATION) {
        /* Continuation frames only extend an open message (checked at
         * header parse). */
    } else if (!ws->frame_fin) {
        ws->in_message = 1;
        ws->msg_op = ws->frame_op;
        return;
    } else {
        /* Unfragmented message: opcode is its own message opcode. */
        ws->msg_op = ws->frame_op;
    }
    if (ws->frame_fin) {
        cfw_wsc_deliver(ws);
    }
}

/* Feeds received bytes through the parser. */
static void cfw_wsc_feed(cfw_ws_client_t *ws, const uint8_t *data,
                         size_t size)
{
    size_t pos = 0;
    size_t need;

    while (pos < size && !ws->closed) {
        if (!ws->have_header) {
            while (pos < size && ws->hdr_len < 2) {
                ws->hdr[ws->hdr_len++] = data[pos++];
            }
            if (ws->hdr_len < 2) {
                return;
            }
            {
                uint8_t b1 = ws->hdr[1];
                uint8_t len7 = b1 & 0x7f;
                /* Masking key only present when MASK is set; masked
                 * frames are a protocol error but must still be parsed
                 * far enough to be rejected. */
                need = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) +
                       ((b1 & 0x80) ? 4 : 0);
            }
            while (pos < size && ws->hdr_len < need) {
                ws->hdr[ws->hdr_len++] = data[pos++];
            }
            if (ws->hdr_len < need) {
                return; /* need more input */
            }
            ws->have_header = 1;
            cfw_wsc_parse_header(ws);
            if (ws->closed) {
                return;
            }
            if (ws->frame_remaining == 0) {
                ws->have_header = 0;
                ws->hdr_len = 0;
                cfw_wsc_frame_done(ws);
                if (ws->closed) {
                    return;
                }
                continue;
            }
        }

        /* Payload state: consume up to frame_remaining bytes. */
        {
            uint64_t take64 = ws->frame_remaining;
            size_t avail = size - pos;
            size_t take;
            if (take64 > (uint64_t)avail) {
                take64 = avail;
            }
            take = (size_t)take64;
            if (take == 0) {
                return;
            }
            if (ws->frame_to_asm) {
                if (cfw_buffer_append(&ws->assembly, data + pos,
                                      take) != CFW_OK) {
                    cfw_wsc_fail(ws, 1011);
                    return;
                }
            } else {
                memcpy(ws->ctrl_buf + ws->ctrl_len, data + pos, take);
                ws->ctrl_len += take;
            }
            ws->frame_remaining -= take64;
            pos += take;
            if (ws->frame_remaining == 0) {
                ws->have_header = 0;
                ws->hdr_len = 0;
                cfw_wsc_frame_done(ws);
                if (ws->closed) {
                    return;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Connect + handshake                                                 */
/* ------------------------------------------------------------------ */

/* Nonblocking connect with deadline: waits for writability then
 * checks SO_ERROR to distinguish success from refusal. */
static cfw_status_t cfw_wsc_connect(cfw_ws_client_t *ws,
                                    const cfw_addr_t *addr)
{
    cfw_status_t status;

    status = cfw_net_socket(cfw_addr_is_ipv6(addr) ? AF_INET6 : AF_INET,
                            SOCK_STREAM, 0, &ws->fd);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_pollset_add(ws->pollset,
                             (intptr_t)cfw_fd_get(&ws->fd),
                             CFW_POLL_IN | CFW_POLL_OUT, NULL);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_net_connect(&ws->fd, addr);
    if (status == CFW_OK) {
        return CFW_OK;
    }
    if (status != CFW_ERROR_WOULDBLOCK) {
        return status;
    }
    for (;;) {
        int sock_err = 0;
        status = cfw_wsc_wait(ws, CFW_POLL_OUT);
        if (status != CFW_OK) {
            return status;
        }
        status = cfw_net_so_error(&ws->fd, &sock_err);
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
static cfw_status_t cfw_wsc_handshake(cfw_ws_client_t *ws)
{
    cfw_status_t status;

    for (;;) {
        status = cfw_tls_handshake(ws->tls);
        if (status == CFW_OK) {
            return CFW_OK;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        status = cfw_wsc_wait(ws, cfw_wsc_tls_events(ws, 0));
        if (status != CFW_OK) {
            return status;
        }
    }
}

/* Reads more bytes into pending, compacting consumed prefix. */
static cfw_status_t cfw_wsc_fill(cfw_ws_client_t *ws,
                                 cfw_buffer_t *pending, size_t *pos,
                                 int *input_closed)
{
    uint8_t buffer[CFW_WSC_READ_BUF];
    size_t got = 0;
    int eof = 0;
    cfw_status_t status;

    for (;;) {
        status = cfw_wsc_read(ws, buffer, sizeof(buffer), &got, &eof);
        if (status == CFW_OK) {
            break;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            return status;
        }
        /* WOULDBLOCK means TLS has decrypted bytes pending; loop. */
    }

    if (eof) {
        *input_closed = 1;
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

/* Validates the 101 response head: required headers and the
 * Sec-WebSocket-Accept known-answer. */
static int cfw_wsc_handshake_response_ok(
    const cfw_http_response_head_t *head, const char *accept)
{
    cfw_strview_t block = head->headers;
    size_t cursor = 0;
    int saw_upgrade = 0;
    int saw_connection = 0;
    int saw_accept = 0;
    cfw_http_header_t header;
    int done = 0;

    if (head->status_code != 101) {
        return 0;
    }
    while (!done) {
        if (cfw_http_headers_next(&block, &cursor, &header, &done) !=
            CFW_OK) {
            return 0;
        }
        if (done) {
            break;
        }
        if (cfw_wsc_value_is(&header.name, "upgrade") &&
            cfw_wsc_value_has_token(&header.value, "websocket")) {
            saw_upgrade = 1;
        } else if (cfw_wsc_value_is(&header.name, "connection") &&
                   cfw_wsc_value_has_token(&header.value, "upgrade")) {
            saw_connection = 1;
        } else if (cfw_wsc_value_is(&header.name,
                                    "sec-websocket-accept")) {
            cfw_strview_t want;
            if (cfw_strview_from_cstr(&want, accept) == CFW_OK) {
                int equal = 0;
                if (cfw_strview_equal_nocase(&header.value, &want,
                                             &equal) == CFW_OK &&
                    equal) {
                    saw_accept = 1;
                }
            }
        }
    }
    return saw_upgrade && saw_connection && saw_accept;
}

/* Builds the HTTP Upgrade request into `out`. */
static cfw_status_t cfw_wsc_handshake_request(
    const cfw_ws_client_options_t *options, const char *key_b64,
    cfw_buffer_t *out)
{
    char line[640];
    int n;
    cfw_status_t st;
    size_t i;

    /* "GET <path> HTTP/1.1\r\n" — path is caller-controlled but was
     * validated to start with '/'; cap keeps the stack buffer bounded
     * (longer paths are rejected rather than truncated). */
    n = snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\n", options->path);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    st = cfw_buffer_append(out, line, (size_t)n);
    if (st != CFW_OK) {
        return st;
    }

    /* Host header: bracket IPv6 literals. */
    {
        char host_hdr[600];
        const char *h = options->host;
        int bracket = h[0] != '[' && strchr(h, ':') != NULL;
        n = snprintf(host_hdr, sizeof(host_hdr), "Host: %s%s%s:%u\r\n",
                     bracket ? "[" : "", h, bracket ? "]" : "",
                     (unsigned)options->port);
        if (n <= 0 || (size_t)n >= sizeof(host_hdr)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        st = cfw_buffer_append(out, host_hdr, (size_t)n);
        if (st != CFW_OK) {
            return st;
        }
    }

    st = cfw_buffer_append(out,
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n",
                           sizeof("Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n") - 1);
    if (st != CFW_OK) {
        return st;
    }
    n = snprintf(line, sizeof(line),
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n",
                 key_b64);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return CFW_ERROR_INTERNAL;
    }
    st = cfw_buffer_append(out, line, (size_t)n);
    if (st != CFW_OK) {
        return st;
    }
    for (i = 0; i < options->header_count; i++) {
        const cfw_http_header_t *h = &options->headers[i];
        if (h->name.data == NULL || h->name.size == 0 ||
            (h->value.size != 0 && h->value.data == NULL)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        st = cfw_buffer_append(out, h->name.data, h->name.size);
        if (st != CFW_OK) {
            return st;
        }
        st = cfw_buffer_append(out, ": ", 2);
        if (st != CFW_OK) {
            return st;
        }
        st = cfw_buffer_append(out, h->value.data, h->value.size);
        if (st != CFW_OK) {
            return st;
        }
        st = cfw_buffer_append(out, "\r\n", 2);
        if (st != CFW_OK) {
            return st;
        }
    }
    return cfw_buffer_append(out, "\r\n", 2);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_ws_client_connect(
    const cfw_ws_client_options_t *options, cfw_ws_client_t **out)
{
    cfw_ws_client_t *ws = NULL;
    cfw_pollset_t *pollset = NULL;
    cfw_tls_conn_t *tls = NULL;
    cfw_buffer_t pending;
    cfw_buffer_t request;
    cfw_http_head_reader_t reader;
    cfw_addr_t addr;
    cfw_addr_family_t families[2];
    size_t family_i;
    size_t pos = 0;
    int input_closed = 0;
    int reader_ready = 0;
    int pending_ready = 0;
    int request_ready = 0;
    cfw_status_t status;
    cfw_strview_t head_view;
    cfw_http_response_head_t resp_head;
    uint8_t key_raw[16];
    char key_b64[CFW_WS_ACCEPT_KEY_CAPACITY + 8];
    char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
    size_t key_len = 0;
    size_t accept_len = 0;

    if (options == NULL || out == NULL || options->host == NULL ||
        options->host[0] == '\0' || options->port == 0 ||
        options->path == NULL || options->path[0] != '/' ||
        options->timeout.nanoseconds < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    {
        size_t i;
        const unsigned char *p;
        /* The request target is emitted verbatim into the request
         * line; apply the same grammar cfw_http_request_head_format
         * enforces so path cannot inject SP/CTL bytes. */
        for (p = (const unsigned char *)options->path; *p != '\0';
             p++) {
            if (*p <= 0x20 || *p == 0x7F) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
        for (i = 0; i < options->header_count; i++) {
            const cfw_http_header_t *h = &options->headers[i];
            if (cfw_wsc_name_forbidden(&h->name) ||
                h->name.data == NULL ||
                !cfw_http_field_name_valid(&h->name) ||
                (h->value.size != 0 && h->value.data == NULL) ||
                !cfw_http_field_value_valid(h->value.data,
                                            h->value.size)) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    *out = NULL;

    ws = calloc(1, sizeof(*ws));
    if (ws == NULL) {
        return CFW_ERROR_NOMEM;
    }
    cfw_fd_init(&ws->fd);
    memset(&pending, 0, sizeof(pending));
    memset(&request, 0, sizeof(request));

    ws->io_timeout = options->timeout;
    if (ws->io_timeout.nanoseconds == 0) {
        status = cfw_duration_from_millis(CFW_WSC_DEFAULT_TIMEOUT_MS,
                                          &ws->io_timeout);
        if (status != CFW_OK) {
            goto fail;
        }
    }
    ws->max_message_size = options->max_message_size != 0
                               ? options->max_message_size
                               : CFW_WSC_DEFAULT_MAX_MESSAGE;
    ws->on_message = options->on_message;
    ws->user = options->user;

    status = cfw_wsc_set_deadline(ws, &ws->io_timeout);
    if (status != CFW_OK) {
        goto fail;
    }

    status = cfw_pollset_create(&pollset);
    if (status != CFW_OK) {
        goto fail;
    }
    ws->pollset = pollset;
    status = cfw_buffer_init(&ws->assembly);
    if (status != CFW_OK) {
        goto fail;
    }

    /* Resolve and connect: IPv4 then IPv6. */
    families[0] = CFW_ADDR_FAMILY_IPV4;
    families[1] = CFW_ADDR_FAMILY_IPV6;
    status = CFW_ERROR_IO;
    for (family_i = 0; family_i < 2; family_i++) {
        cfw_status_t attempt;
        attempt = cfw_addr_resolve(&addr, options->host,
                                   options->port, families[family_i]);
        if (attempt != CFW_OK) {
            continue;
        }
        attempt = cfw_wsc_connect(ws, &addr);
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
            pollset, (intptr_t)cfw_fd_get(&ws->fd));
        cfw_fd_close(&ws->fd);
        cfw_fd_init(&ws->fd);
    }
    if (status != CFW_OK) {
        goto fail;
    }

    /* Optional TLS layer (wss://). */
    if (options->tls_ctx != NULL) {
        status = cfw_tls_conn_create(options->tls_ctx,
                                     cfw_fd_get(&ws->fd), &tls);
        if (status != CFW_OK) {
            goto fail;
        }
        ws->tls = tls;
        status = cfw_tls_conn_set_server_name(tls, options->host);
        if (status != CFW_OK) {
            goto fail;
        }
        status = cfw_wsc_handshake(ws);
        if (status != CFW_OK) {
            goto fail;
        }
    }

    /* Sec-WebSocket-Key: 16 random bytes, base64-encoded. */
    status = cfw_random_bytes(key_raw, sizeof(key_raw));
    if (status != CFW_OK) {
        goto fail;
    }
    status = cfw_base64_encode(key_raw, sizeof(key_raw), key_b64,
                               sizeof(key_b64), &key_len);
    if (status != CFW_OK) {
        goto fail;
    }
    /* Expected accept = base64(SHA1(key || GUID)). */
    {
        cfw_ws_sha1_t sha;
        uint8_t digest[20];
        cfw_ws_sha1_init(&sha);
        cfw_ws_sha1_update(&sha, (const uint8_t *)key_b64, key_len);
        cfw_ws_sha1_update(&sha, (const uint8_t *)CFW_WSC_WS_GUID,
                           sizeof(CFW_WSC_WS_GUID) - 1);
        cfw_ws_sha1_final(&sha, digest);
        status = cfw_base64_encode(digest, sizeof(digest), accept,
                                   sizeof(accept), &accept_len);
        if (status != CFW_OK || accept_len == 0) {
            goto fail;
        }
    }

    /* Build and send the Upgrade request. */
    status = cfw_buffer_init(&request);
    if (status != CFW_OK) {
        goto fail;
    }
    request_ready = 1;
    status = cfw_wsc_handshake_request(options, key_b64, &request);
    if (status != CFW_OK) {
        goto fail;
    }
    status = cfw_wsc_write_all(ws, request.data, request.size);
    if (status != CFW_OK) {
        goto fail;
    }
    cfw_buffer_free(&request);
    request_ready = 0;

    /* Read the response head (skipping interim 1xx except 101). */
    status = cfw_buffer_init(&pending);
    if (status != CFW_OK) {
        goto fail;
    }
    pending_ready = 1;
    status = cfw_http_head_reader_init(&reader, CFW_WSC_MAX_HEAD);
    if (status != CFW_OK) {
        goto fail;
    }
    reader_ready = 1;
    for (;;) {
        while (!cfw_http_head_reader_is_complete(&reader)) {
            cfw_span_t input;
            size_t consumed = 0;

            if (input_closed) {
                status = CFW_ERROR_IO;
                goto fail;
            }
            status = cfw_wsc_fill(ws, &pending, &pos, &input_closed);
            if (status != CFW_OK) {
                goto fail;
            }
            if (input_closed && pending.size == pos) {
                status = CFW_ERROR_IO;
                goto fail;
            }
            input.data = pending.size > pos ? pending.data + pos : NULL;
            input.size = pending.size - pos;
            status = cfw_http_head_reader_feed(&reader, &input,
                                               &consumed);
            if (status != CFW_OK) {
                goto fail;
            }
            pos += consumed;
        }

        status = cfw_http_head_reader_head(&reader, &head_view);
        if (status != CFW_OK) {
            goto fail;
        }
        memset(&resp_head, 0, sizeof(resp_head));
        status = cfw_http_response_head_parse(&head_view, &resp_head);
        if (status != CFW_OK) {
            status = CFW_ERROR_IO;
            goto fail;
        }
        if (resp_head.status_code >= 100 &&
            resp_head.status_code <= 199 &&
            resp_head.status_code != 101) {
            cfw_http_head_reader_reset(&reader);
            continue;
        }
        break;
    }

    if (!cfw_wsc_handshake_response_ok(&resp_head, accept)) {
        status = CFW_ERROR_IO;
        goto fail;
    }

    /* Bytes past the head are WebSocket frames; feed the parser. */
    if (pos < pending.size) {
        cfw_wsc_feed(ws, pending.data + pos, pending.size - pos);
        if (ws->closed) {
            status = CFW_ERROR_IO;
            goto fail;
        }
    }

    if (reader_ready) {
        cfw_http_head_reader_free(&reader);
        reader_ready = 0;
    }
    if (pending_ready) {
        cfw_buffer_free(&pending);
        pending_ready = 0;
    }
    *out = ws;
    return CFW_OK;

fail:
    if (reader_ready) {
        cfw_http_head_reader_free(&reader);
    }
    if (pending_ready) {
        cfw_buffer_free(&pending);
    }
    if (request_ready) {
        cfw_buffer_free(&request);
    }
    if (tls != NULL) {
        cfw_tls_conn_destroy(tls);
    }
    if (pollset != NULL) {
        cfw_pollset_destroy(pollset);
    }
    if (ws != NULL) {
        cfw_buffer_free(&ws->assembly);
        cfw_fd_close(&ws->fd);
        free(ws);
    }
    return status;
}

void cfw_ws_client_destroy(cfw_ws_client_t *ws)
{
    if (ws == NULL) {
        return;
    }
    cfw_buffer_free(&ws->assembly);
    if (ws->tls != NULL) {
        cfw_tls_conn_destroy(ws->tls);
    }
    cfw_fd_close(&ws->fd);
    if (ws->pollset != NULL) {
        cfw_pollset_destroy(ws->pollset);
    }
    free(ws);
}

int cfw_ws_client_is_closed(const cfw_ws_client_t *ws)
{
    return ws != NULL && ws->closed;
}

cfw_status_t cfw_ws_client_send(cfw_ws_client_t *ws,
                                cfw_ws_opcode_t opcode,
                                const void *data, size_t size)
{
    cfw_status_t status;

    if (ws == NULL || ws->closed || ws->close_sent) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size != 0 && data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Fresh per-call deadline for the bounded write. */
    status = cfw_wsc_set_deadline(ws, &ws->io_timeout);
    if (status != CFW_OK) {
        return status;
    }
    switch (opcode) {
    case CFW_WS_OPCODE_TEXT:
    case CFW_WS_OPCODE_BINARY:
        status = cfw_wsc_send_data(ws, opcode, data, size);
        break;
    case CFW_WS_OPCODE_PING:
    case CFW_WS_OPCODE_PONG:
        if (size > CFW_WSC_MAX_CTRL) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        status = cfw_wsc_send_frame(ws, 1, opcode, data, size);
        break;
    case CFW_WS_OPCODE_CLOSE:
        if (size > CFW_WSC_MAX_CTRL) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        /* The payload is a raw close body: empty, or a 2-byte status
         * (cfw_ws_close_status_ok) plus an optional UTF-8 reason.
         * Validate it here so the generic path cannot emit a close
         * frame a peer must protocol-fail. */
        if (size == 1) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (size >= 2) {
            const uint8_t *p = (const uint8_t *)data;
            uint16_t close_status = (uint16_t)((p[0] << 8) | p[1]);
            if (!cfw_ws_close_status_ok(close_status) ||
                (size > 2 &&
                 !cfw_ws_utf8_valid(p + 2, size - 2))) {
                return CFW_ERROR_INVALID_ARGUMENT;
            }
        }
        status = cfw_wsc_send_frame(ws, 1, opcode, data, size);
        if (status == CFW_OK) {
            ws->close_sent = 1;
        }
        break;
    default:
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return status;
}

cfw_status_t cfw_ws_client_send_text(cfw_ws_client_t *ws,
                                     const void *data, size_t size)
{
    return cfw_ws_client_send(ws, CFW_WS_OPCODE_TEXT, data, size);
}

cfw_status_t cfw_ws_client_send_binary(cfw_ws_client_t *ws,
                                       const void *data, size_t size)
{
    return cfw_ws_client_send(ws, CFW_WS_OPCODE_BINARY, data, size);
}

cfw_status_t cfw_ws_client_send_ping(cfw_ws_client_t *ws,
                                     const void *data, size_t size)
{
    return cfw_ws_client_send(ws, CFW_WS_OPCODE_PING, data, size);
}

cfw_status_t cfw_ws_client_send_pong(cfw_ws_client_t *ws,
                                     const void *data, size_t size)
{
    return cfw_ws_client_send(ws, CFW_WS_OPCODE_PONG, data, size);
}

cfw_status_t cfw_ws_client_send_close(cfw_ws_client_t *ws,
                                      uint16_t status,
                                      const char *reason,
                                      size_t reason_size)
{
    uint8_t payload[2 + 123];
    size_t n = 0;
    cfw_status_t st;

    if (ws == NULL || ws->closed || ws->close_sent || reason == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    /* Fresh per-call deadline, matching cfw_ws_client_send. */
    st = cfw_wsc_set_deadline(ws, &ws->io_timeout);
    if (st != CFW_OK) {
        return st;
    }
    if (status != 0) {
        if (!cfw_ws_close_status_ok(status) ||
            reason_size > sizeof(payload) - 2) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (reason_size != 0 &&
            !cfw_ws_utf8_valid((const uint8_t *)reason, reason_size)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        payload[0] = (uint8_t)(status >> 8);
        payload[1] = (uint8_t)status;
        n = 2;
        if (reason_size != 0) {
            memcpy(payload + 2, reason, reason_size);
            n += reason_size;
        }
    } else if (reason_size != 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    st = cfw_wsc_send_frame(ws, 1, CFW_WS_OPCODE_CLOSE, payload, n);
    if (st == CFW_OK) {
        ws->close_sent = 1;
    }
    return st;
}

cfw_status_t cfw_ws_client_pump(cfw_ws_client_t *ws,
                                const cfw_duration_t *timeout)
{
    uint8_t buffer[CFW_WSC_READ_BUF];
    cfw_status_t status;
    size_t got = 0;
    int eof = 0;

    if (ws == NULL || ws->closed) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    status = cfw_wsc_set_deadline(ws, timeout);
    if (status != CFW_OK) {
        return status;
    }

    /* One bounded read per pump: waits up to `timeout` for the first
     * bytes, feeds everything read through the parser, and returns.
     * Callers loop pump() for continuous service. */
    for (;;) {
        status = cfw_wsc_read(ws, buffer, sizeof(buffer), &got, &eof);
        if (status == CFW_ERROR_WOULDBLOCK) {
            /* TLS has decrypted bytes pending; loop back and read. */
            continue;
        }
        break;
    }
    if (status != CFW_OK) {
        ws->closed = 1;
        return status;
    }
    if (eof) {
        ws->closed = 1;
        return CFW_OK;
    }
    cfw_wsc_feed(ws, buffer, got);
    return CFW_OK;
}

