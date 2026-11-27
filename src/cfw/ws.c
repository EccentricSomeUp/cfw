/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/ws.h>

#include "private/ws_proto.h"

#include <cfw/codec.h>
#include <cfw/http.h>
#include <cfw/strview.h>

#include <string.h>

/* ------------------------------------------------------------------ */
/* Header helpers                                                      */
/* ------------------------------------------------------------------ */

static int cfw_ws_header_is(const cfw_http_header_t *header,
                            const char *name)
{
    cfw_strview_t hn;
    int eq = 0;
    cfw_strview_from_cstr(&hn, name);
    if (cfw_strview_equal_nocase(&header->name, &hn, &eq) != CFW_OK ||
        !eq) {
        return 0;
    }
    return 1;
}

/* Returns the header value span for `name` (first match), or NULL. */
static const cfw_strview_t *cfw_ws_header_find(const cfw_request_t *req,
                                               const char *name)
{
    size_t i;
    for (i = 0; i < req->header_count; i++) {
        if (cfw_ws_header_is(&req->headers[i], name)) {
            return &req->headers[i].value;
        }
    }
    return NULL;
}

/* Case-insensitive token membership in a comma-separated header value
 * (OWS trimmed), e.g. Connection: keep-alive, Upgrade. */
static int cfw_ws_value_has_token(const cfw_strview_t *value,
                                  const char *token)
{
    cfw_strview_t wanted;
    size_t pos = 0;
    cfw_strview_from_cstr(&wanted, token);

    while (pos <= value->size) {
        size_t ts = pos;
        size_t te;
        cfw_strview_t element;
        int eq = 0;
        while (pos < value->size && value->data[pos] != ',') {
            pos++;
        }
        te = pos;
        if (pos < value->size) {
            pos++;
        }
        while (ts < te &&
               (value->data[ts] == ' ' || value->data[ts] == '\t')) {
            ts++;
        }
        while (te > ts &&
               (value->data[te - 1] == ' ' || value->data[te - 1] == '\t')) {
            te--;
        }
        cfw_strview_init(&element, value->data + ts, te - ts);
        if (cfw_strview_equal_nocase(&element, &wanted, &eq) == CFW_OK &&
            eq) {
            return 1;
        }
        if (pos >= value->size) {
            break;
        }
    }
    return 0;
}

static int cfw_ws_strview_is(const cfw_strview_t *view, const char *text)
{
    cfw_strview_t wanted;
    int eq = 0;
    cfw_strview_from_cstr(&wanted, text);
    return cfw_strview_equal_nocase(view, &wanted, &eq) == CFW_OK && eq;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

cfw_status_t cfw_ws_handshake_validate(const cfw_request_t *req)
{
    const cfw_strview_t *upgrade;
    const cfw_strview_t *connection;
    const cfw_strview_t *version;
    const cfw_strview_t *key;
    uint8_t decoded[16];

    if (req == NULL || req->method != CFW_HTTP_METHOD_GET) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    upgrade = cfw_ws_header_find(req, "upgrade");
    if (upgrade == NULL || !cfw_ws_strview_is(upgrade, "websocket")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    connection = cfw_ws_header_find(req, "connection");
    if (connection == NULL ||
        !cfw_ws_value_has_token(connection, "upgrade")) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    version = cfw_ws_header_find(req, "sec-websocket-version");
    if (version == NULL || version->size != 2 ||
        !(version->data[0] == '1' && version->data[1] == '3')) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    key = cfw_ws_header_find(req, "sec-websocket-key");
    if (key == NULL || key->size == 0 || key->size > 256) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    {
        cfw_strview_t key_view;
        size_t decoded_size = 0;

        if (cfw_strview_init(&key_view, key->data, key->size) != CFW_OK ||
            cfw_base64_decode(&key_view, decoded, sizeof(decoded),
                              &decoded_size) != CFW_OK ||
            decoded_size != sizeof(decoded)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    }
    return CFW_OK;
}

cfw_status_t cfw_ws_accept_key(const char *key, size_t key_size, char *out,
                               size_t out_size)
{
    static const char guid[] =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    cfw_ws_sha1_t sha;
    uint8_t digest[20];
    char material[256 + sizeof(guid) - 1];
    size_t mlen;

    if (key == NULL || key_size == 0 || key_size > 256 || out == NULL ||
        out_size < CFW_WS_ACCEPT_KEY_CAPACITY) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    mlen = key_size;
    memcpy(material, key, key_size);
    memcpy(material + key_size, guid, sizeof(guid) - 1);
    mlen += sizeof(guid) - 1;

    cfw_ws_sha1_init(&sha);
    cfw_ws_sha1_update(&sha, (const uint8_t *)material, mlen);
    cfw_ws_sha1_final(&sha, digest);
    return cfw_base64_encode(digest, sizeof(digest), out, out_size, NULL);
}

cfw_status_t cfw_ws_handshake_accept(const cfw_request_t *req, char *out,
                                     size_t out_size)
{
    const cfw_strview_t *key;

    if (req == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    key = cfw_ws_header_find(req, "sec-websocket-key");
    if (key == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return cfw_ws_accept_key(key->data, key->size, out, out_size);
}

/* ------------------------------------------------------------------ */
/* Frame codec (RFC 6455 section 5)                                    */
/* ------------------------------------------------------------------ */

#include <cfw/buffer.h>
#include <cfw/span.h>

#include <stdlib.h>

#define CFW_WS_DEFAULT_MAX_MESSAGE (1024u * 1024u)
#define CFW_WS_DEFAULT_MAX_FRAME   16384u
#define CFW_WS_MAX_CTRL_PAYLOAD    125u
#define CFW_WS_HDR_CAP             14u
#define CFW_WS_READ_BUF            16384u

struct cfw_ws {
    cfw_upgraded_conn_t *conn;
    cfw_ws_message_fn on_message;
    void *user;
    size_t max_message_size;
    size_t max_frame_payload;
    cfw_buffer_t assembly;

    /* Incremental frame parser. */
    int have_header;      /* full header bytes staged in hdr[] */
    size_t hdr_len;
    uint8_t hdr[CFW_WS_HDR_CAP];
    /* Current frame fields (valid while consuming payload). */
    uint64_t frame_remaining;
    int frame_fin;
    cfw_ws_opcode_t frame_op;
    int frame_to_asm;
    uint8_t frame_mask[4];
    unsigned frame_mask_off;
    uint8_t ctrl_buf[CFW_WS_MAX_CTRL_PAYLOAD];
    size_t ctrl_len;
    /* Reassembly state. */
    int in_message;
    cfw_ws_opcode_t msg_op;
    /* Lifecycle + pending control frame (pong/close echo). */
    int closing;
    int closed;
    int has_pending_ctrl;
    uint8_t pending_ctrl_op;
    uint8_t pending_ctrl[CFW_WS_MAX_CTRL_PAYLOAD];
    uint16_t pending_ctrl_len;
};

enum {
    CFW_WS_ST_HEADER = 0,
    CFW_WS_ST_PAYLOAD
};

static size_t cfw_ws_frame_hdr_len(uint64_t payload_len)
{
    /* Server->client frames are never masked: 2-byte base + ext. */
    if (payload_len < 126) {
        return 2;
    }
    if (payload_len <= 0xffff) {
        return 4;
    }
    return 10;
}

/* Writes the frame header for an unmasked server frame into out
 * (must hold >= 10 bytes). Returns header length. */
static size_t cfw_ws_frame_hdr_write(uint8_t *out, int fin,
                                     cfw_ws_opcode_t opcode,
                                     uint64_t payload_len)
{
    size_t n = 0;
    out[n++] = (uint8_t)((fin ? 0x80 : 0) | (uint8_t)opcode);
    if (payload_len < 126) {
        out[n++] = (uint8_t)payload_len;
    } else if (payload_len <= 0xffff) {
        out[n++] = 126;
        out[n++] = (uint8_t)(payload_len >> 8);
        out[n++] = (uint8_t)payload_len;
    } else {
        int i;
        out[n++] = 127;
        for (i = 7; i >= 0; i--) {
            out[n++] = (uint8_t)(payload_len >> (i * 8));
        }
    }
    return n;
}

/* Queues one complete frame (header + payload) via the upgraded
 * connection. The caller must have verified the bytes fit
 * cfw_upgraded_out_free. */
static cfw_status_t cfw_ws_queue_frame(cfw_ws_t *ws, int fin,
                                       cfw_ws_opcode_t opcode,
                                       const void *data, size_t size)
{
    uint8_t hdr[10];
    size_t hn = cfw_ws_frame_hdr_write(hdr, fin, opcode, (uint64_t)size);
    cfw_status_t st;

    /* Atomicity: never emit a header whose payload cannot follow. */
    if ((uint64_t)hn + (uint64_t)size >
        (uint64_t)cfw_upgraded_out_free(ws->conn)) {
        return CFW_ERROR_WOULDBLOCK;
    }
    st = cfw_upgraded_send(ws->conn, hdr, hn);
    if (st != CFW_OK) {
        return st;
    }
    if (size != 0) {
        return cfw_upgraded_send(ws->conn, data, size);
    }
    return CFW_OK;
}

/* Begins the close sequence: queues a close frame carrying `status`
 * (0 = no status, empty payload) when possible, then closes the
 * connection once output drains. Idempotent. */
static void cfw_ws_fail(cfw_ws_t *ws, uint16_t status)
{
    uint8_t payload[2];
    size_t plen = 0;

    if (ws->closing) {
        return;
    }
    ws->closing = 1;
    ws->closed = 1;
    if (status != 0) {
        payload[0] = (uint8_t)(status >> 8);
        payload[1] = (uint8_t)status;
        plen = 2;
    }
    (void)cfw_ws_queue_frame(ws, 1, CFW_WS_OPCODE_CLOSE, payload, plen);
    cfw_upgraded_close(ws->conn);
}

/* Queues or stashes a control frame for later delivery. Used for
 * automatic pong replies and close echoes where the app is not in the
 * send path. */
static void cfw_ws_queue_control(cfw_ws_t *ws, cfw_ws_opcode_t opcode,
                                 const void *data, size_t size)
{
    if (size > CFW_WS_MAX_CTRL_PAYLOAD || ws->closed) {
        return;
    }
    /* Try direct send first when the queue has room for the whole
     * frame; otherwise stash for the next pump. */
    if (cfw_upgraded_out_free(ws->conn) >=
        cfw_ws_frame_hdr_len((uint64_t)size) + size) {
        (void)cfw_ws_queue_frame(ws, 1, opcode, data, size);
        return;
    }
    if (ws->has_pending_ctrl) {
        return; /* one slot only; drop under backpressure */
    }
    memcpy(ws->pending_ctrl, data, size);
    ws->pending_ctrl_len = (uint16_t)size;
    ws->pending_ctrl_op = (uint8_t)opcode;
    ws->has_pending_ctrl = 1;
}

static void cfw_ws_pump_control(cfw_ws_t *ws)
{
    if (!ws->has_pending_ctrl || ws->closed) {
        return;
    }
    {
        uint8_t hdr[10];
        size_t hn = cfw_ws_frame_hdr_write(
            hdr, 1, (cfw_ws_opcode_t)ws->pending_ctrl_op,
            ws->pending_ctrl_len);
        cfw_status_t st;
        if (cfw_upgraded_out_free(ws->conn) < hn + ws->pending_ctrl_len) {
            return;
        }
        st = cfw_upgraded_send(ws->conn, hdr, hn);
        if (st == CFW_OK && ws->pending_ctrl_len != 0) {
            st = cfw_upgraded_send(ws->conn, ws->pending_ctrl,
                                   ws->pending_ctrl_len);
        }
        if (st == CFW_OK) {
            ws->has_pending_ctrl = 0;
        }
    }
}

/* Parses a fully-staged header (hdr_len == hdr_need at entry handled
 * by caller) and validates it. Returns CFW_OK and fills frame fields,
 * or a protocol failure via cfw_ws_fail. */
static void cfw_ws_parse_header(cfw_ws_t *ws)
{
    const uint8_t *h = ws->hdr;
    uint8_t b0 = h[0];
    uint8_t b1 = h[1];
    unsigned opcode = b0 & 0x0f;
    int masked = (b1 & 0x80) != 0;
    uint64_t len;
    size_t ext = 0;
    size_t pos = 2;

    ws->frame_fin = (b0 & 0x80) != 0;
    ws->frame_op = (cfw_ws_opcode_t)opcode;

    /* RSV bits must be zero (no extensions negotiated). */
    if ((b0 & 0x70) != 0) {
        cfw_ws_fail(ws, 1002);
        return;
    }
    /* Client->server frames must be masked. */
    if (!masked) {
        cfw_ws_fail(ws, 1002);
        return;
    }
    switch (opcode) {
    case CFW_WS_OPCODE_CONTINUATION:
        if (!ws->in_message) {
            cfw_ws_fail(ws, 1002);
            return;
        }
        break;
    case CFW_WS_OPCODE_TEXT:
    case CFW_WS_OPCODE_BINARY:
        if (ws->in_message) {
            cfw_ws_fail(ws, 1002);
            return;
        }
        break;
    case CFW_WS_OPCODE_CLOSE:
    case CFW_WS_OPCODE_PING:
    case CFW_WS_OPCODE_PONG:
        if (!ws->frame_fin) {
            cfw_ws_fail(ws, 1002);
            return;
        }
        break;
    default:
        cfw_ws_fail(ws, 1002);
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
            cfw_ws_fail(ws, 1002); /* length MSB must be 0 */
            return;
        }
        len = 0;
        for (i = 0; i < 8; i++) {
            len = (len << 8) | h[pos + i];
        }
        pos += 8;
    }
    /* Masking key follows the (optional) extended length. */
    memcpy(ws->frame_mask, h + pos, 4);
    ws->frame_mask_off = 0;
    pos += 4;
    (void)pos;

    /* Control frames are limited to 125 bytes of payload. */
    if (opcode >= CFW_WS_OPCODE_CLOSE && len > CFW_WS_MAX_CTRL_PAYLOAD) {
        cfw_ws_fail(ws, 1002);
        return;
    }
    /* Data frames must fit the message bound including prior
     * fragments. */
    if (opcode <= CFW_WS_OPCODE_BINARY) {
        if (len > (uint64_t)ws->max_message_size ||
            ws->assembly.size >
                ws->max_message_size - (size_t)len) {
            cfw_ws_fail(ws, 1009);
            return;
        }
    }
    ws->frame_remaining = len;
    ws->ctrl_len = 0;
    ws->frame_to_asm = opcode <= CFW_WS_OPCODE_BINARY;
}

/* Handles a fully-received control payload (close/ping/pong). */
static void cfw_ws_handle_ctrl(cfw_ws_t *ws)
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
            } else if (n > 2 &&
                       !cfw_ws_utf8_valid(p + 2, n - 2)) {
                cfw_ws_fail(ws, 1007);
                return;
            }
        }
        if (!valid) {
            cfw_ws_fail(ws, 1002);
            return;
        }
        /* Echo the close payload unless we already sent one, then
         * close the transport once output drains. */
        if (!ws->closing) {
            cfw_ws_queue_control(ws, CFW_WS_OPCODE_CLOSE, p, n);
        }
        ws->closing = 1;
        ws->closed = 1;
        cfw_upgraded_close(ws->conn);
        return;
    }
    case CFW_WS_OPCODE_PING:
        cfw_ws_queue_control(ws, CFW_WS_OPCODE_PONG, p, n);
        return;
    case CFW_WS_OPCODE_PONG:
    default:
        return;
    }
}

/* Delivers a completed data message to the application. */
static void cfw_ws_deliver(cfw_ws_t *ws)
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
        cfw_ws_fail(ws, 1007);
        return;
    }
    ws->assembly.size = 0;
    ws->in_message = 0;
    if (ws->on_message != NULL) {
        st = ws->on_message(ws, &msg, ws->user);
        if (st != CFW_OK) {
            cfw_ws_fail(ws, 1011);
        }
    }
}

/* Called when a frame's payload has been fully consumed. */
static void cfw_ws_frame_done(cfw_ws_t *ws)
{
    if (!ws->frame_to_asm) {
        cfw_ws_handle_ctrl(ws);
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
        cfw_ws_deliver(ws);
    }
}

/* Feeds received bytes through the parser. `data` is mutable: payload
 * is unmasked in place before being staged. */
static void cfw_ws_feed(cfw_ws_t *ws, uint8_t *data, size_t size)
{
    size_t pos = 0;
    size_t need;

    while (pos < size && !ws->closed) {
        if (!ws->have_header) {
            /* Stage at least the 2-byte fixed part, then compute the
             * full header length from the length bits and stage the
             * rest (extended length + masking key). hdr_len persists
             * across feed calls. */
            while (pos < size && ws->hdr_len < 2) {
                ws->hdr[ws->hdr_len++] = data[pos++];
            }
            if (ws->hdr_len < 2) {
                return;
            }
            {
                uint8_t b1 = ws->hdr[1];
                uint8_t len7 = b1 & 0x7f;
                /* The masking key is only present when MASK is set;
                 * unmasked frames are a protocol error but must still
                 * be *parsed* far enough to be rejected. */
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
            cfw_ws_parse_header(ws);
            if (ws->closed) {
                return;
            }
            if (ws->frame_remaining == 0) {
                ws->have_header = 0;
                ws->hdr_len = 0;
                cfw_ws_frame_done(ws);
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
            /* Unmask in place: mask index continues across chunks. */
            {
                size_t i;
                for (i = 0; i < take; i++) {
                    data[pos + i] ^= ws->frame_mask[ws->frame_mask_off];
                    ws->frame_mask_off = (ws->frame_mask_off + 1) & 3;
                }
            }
            if (ws->frame_to_asm) {
                if (cfw_buffer_append(&ws->assembly, data + pos,
                                      take) != CFW_OK) {
                    cfw_ws_fail(ws, 1011);
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
                cfw_ws_frame_done(ws);
                if (ws->closed) {
                    return;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

cfw_ws_t *cfw_ws_create(cfw_upgraded_conn_t *conn,
                        const cfw_ws_options_t *options)
{
    cfw_ws_t *ws;

    if (conn == NULL) {
        return NULL;
    }
    ws = calloc(1, sizeof(*ws));
    if (ws == NULL) {
        return NULL;
    }
    if (cfw_buffer_init(&ws->assembly) != CFW_OK) {
        free(ws);
        return NULL;
    }
    ws->conn = conn;
    ws->max_message_size = CFW_WS_DEFAULT_MAX_MESSAGE;
    ws->max_frame_payload = CFW_WS_DEFAULT_MAX_FRAME;
    if (options != NULL) {
        if (options->on_message != NULL) {
            ws->on_message = options->on_message;
        }
        ws->user = options->user;
        if (options->max_message_size != 0) {
            ws->max_message_size = options->max_message_size;
        }
        if (options->max_frame_payload != 0) {
            ws->max_frame_payload = options->max_frame_payload;
        }
    }
    return ws;
}

void cfw_ws_destroy(cfw_ws_t *ws)
{
    if (ws == NULL) {
        return;
    }
    cfw_buffer_free(&ws->assembly);
    free(ws);
}

cfw_status_t cfw_ws_on_event(cfw_ws_t *ws, uint32_t events)
{
    uint8_t buf[CFW_WS_READ_BUF];
    (void)events; /* input is always drained; see header docs */

    if (ws == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (ws->closed) {
        return CFW_OK;
    }
    cfw_ws_pump_control(ws);
    for (;;) {
        size_t n = 0;
        cfw_status_t st = cfw_upgraded_recv(ws->conn, buf, sizeof(buf),
                                            &n);
        if (st == CFW_ERROR_WOULDBLOCK) {
            break;
        }
        if (st != CFW_OK || n == 0) {
            /* Socket error or peer EOF: nothing more to parse. */
            ws->closed = 1;
            cfw_upgraded_close(ws->conn);
            return CFW_OK;
        }
        cfw_ws_feed(ws, buf, n);
        if (ws->closed) {
            break;
        }
    }
    cfw_ws_pump_control(ws);
    return CFW_OK;
}

cfw_status_t cfw_ws_send(cfw_ws_t *ws, cfw_ws_opcode_t opcode,
                         const void *data, size_t size)
{
    size_t offset = 0;
    size_t chunk;
    uint64_t total = 0;
    size_t frames;
    size_t i;
    uint64_t free_space;

    if (ws == NULL || ws->closed || ws->closing ||
        (opcode != CFW_WS_OPCODE_TEXT && opcode != CFW_WS_OPCODE_BINARY) ||
        (size != 0 && data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    /* Pre-flight: accumulate wire bytes and bail as soon as the total
     * exceeds the send queue, so a large message never partially
     * queues. Saturation keeps the sum overflow-free. */
    frames = size == 0 ? 1
                       : size / ws->max_frame_payload +
                             (size % ws->max_frame_payload != 0);
    free_space = (uint64_t)cfw_upgraded_out_free(ws->conn);
    for (i = 0; i < frames; i++) {
        size_t piece = size == 0
                           ? 0
                           : (i == frames - 1
                                  ? size - i * ws->max_frame_payload
                                  : ws->max_frame_payload);
        uint64_t add = (uint64_t)piece +
                       (uint64_t)cfw_ws_frame_hdr_len((uint64_t)piece);
        if (add > UINT64_MAX - total) {
            total = UINT64_MAX;
        } else {
            total += add;
        }
        if (total > free_space) {
            return CFW_ERROR_WOULDBLOCK;
        }
    }

    /* Queue frames one at a time; the pre-flight guarantees each fits. */
    for (;;) {
        int last;
        chunk = ws->max_frame_payload;
        if (size - offset < chunk) {
            chunk = size - offset;
        }
        last = (offset + chunk == size);
        {
            cfw_ws_opcode_t op =
                offset == 0 ? opcode : CFW_WS_OPCODE_CONTINUATION;
            cfw_status_t st =
                cfw_ws_queue_frame(
                    ws, last ? 1 : 0, op,
                    chunk == 0 ? NULL
                               : (const uint8_t *)data + offset,
                    chunk);
            if (st != CFW_OK) {
                return st;
            }
        }
        offset += chunk;
        if (last) {
            break;
        }
    }
    return CFW_OK;
}

cfw_status_t cfw_ws_send_text(cfw_ws_t *ws, const void *data, size_t size)
{
    return cfw_ws_send(ws, CFW_WS_OPCODE_TEXT, data, size);
}

cfw_status_t cfw_ws_send_binary(cfw_ws_t *ws, const void *data,
                                size_t size)
{
    return cfw_ws_send(ws, CFW_WS_OPCODE_BINARY, data, size);
}

static cfw_status_t cfw_ws_send_control_pub(cfw_ws_t *ws,
                                            cfw_ws_opcode_t opcode,
                                            const void *data, size_t size)
{
    size_t total;

    if (ws == NULL || ws->closed || ws->closing ||
        size > CFW_WS_MAX_CTRL_PAYLOAD || (size != 0 && data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    total = cfw_ws_frame_hdr_len((uint64_t)size) + size;
    if (total > cfw_upgraded_out_free(ws->conn)) {
        return CFW_ERROR_WOULDBLOCK;
    }
    return cfw_ws_queue_frame(ws, 1, opcode, data, size);
}

cfw_status_t cfw_ws_send_ping(cfw_ws_t *ws, const void *data, size_t size)
{
    return cfw_ws_send_control_pub(ws, CFW_WS_OPCODE_PING, data, size);
}

cfw_status_t cfw_ws_send_pong(cfw_ws_t *ws, const void *data, size_t size)
{
    return cfw_ws_send_control_pub(ws, CFW_WS_OPCODE_PONG, data, size);
}

cfw_status_t cfw_ws_send_close(cfw_ws_t *ws, uint16_t status,
                               const char *reason, size_t reason_size)
{
    uint8_t payload[2 + CFW_WS_MAX_CTRL_PAYLOAD - 2];
    size_t plen = 0;

    if (ws == NULL || (reason_size != 0 && reason == NULL) ||
        reason_size > CFW_WS_MAX_CTRL_PAYLOAD - 2) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (reason != NULL && reason_size != 0 &&
        !cfw_ws_utf8_valid((const uint8_t *)reason, reason_size)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (status == 0) {
        /* An empty close carries no status; a reason without a
         * status would be misparsed as one by the peer. */
        if (reason_size != 0) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
    } else {
        if (!cfw_ws_close_status_ok(status)) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        payload[0] = (uint8_t)(status >> 8);
        payload[1] = (uint8_t)status;
        plen = 2;
    }
    if (reason_size != 0) {
        memcpy(payload + plen, reason, reason_size);
        plen += reason_size;
    }
    {
        cfw_status_t st =
            cfw_ws_send_control_pub(ws, CFW_WS_OPCODE_CLOSE, payload,
                                    plen);
        if (st == CFW_OK) {
            ws->closing = 1;
            ws->closed = 1;
            cfw_upgraded_close(ws->conn);
        }
        return st;
    }
}

int cfw_ws_is_closed(const cfw_ws_t *ws)
{
    return ws == NULL || ws->closed;
}
