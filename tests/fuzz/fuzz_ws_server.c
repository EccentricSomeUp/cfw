/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the server-side WebSocket frame
 * parser (ADR 0077): a real cfw_server_t with the ws upgrade hook
 * runs on a cfw_thread while the fake client performs the RFC 6455
 * handshake over loopback TCP and then streams frames built from the
 * fuzz input.
 *
 * Input format — a sequence of frames, each:
 *   [b0: FIN|RSV|opcode bits exactly as on the wire]
 *   [b1: 0-125 = length; 126 = u16 BE follows; 127 = u64 BE follows]
 *   [payload bytes; a truncated tail is sent as a short payload]
 *
 * The client re-frames each record as a properly masked WebSocket
 * frame (clients MUST mask, RFC 6455 5.1), preserving b0 and the
 * declared length verbatim — so RSV bits, reserved/invalid opcodes,
 * all length forms, fragmentation/continuation sequences, interleaved
 * control frames, oversized declared lengths, and truncated payloads
 * all reach the server parser. The mask key rotates per frame from a
 * deterministic PRNG seeded by the input.
 */

#include "test_loopback.h"

#include <cfw/cfw.h>
#include <cfw/ws.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CFW_FUZZ_WSS_MAX_INPUT (32u * 1024u)
#define CFW_FUZZ_WSS_MAX_FRAMES 512

typedef struct {
    cfw_arena_t *arena;
    cfw_router_t router;
    cfw_dispatch_t dispatch;
    cfw_server_t server;
    cfw_ws_t *ws;
    /* Per-connection accept key written by the HTTP handler; the
     * response head strview references it after the handler returns,
     * so it must outlive the call (same pattern as test_ws.c). */
    char accept[CFW_WS_ACCEPT_KEY_CAPACITY];
} cfw_fuzz_wss_state_t;

static cfw_status_t cfw_fuzz_wss_http_handler(cfw_request_t *req,
                                              cfw_response_t *res,
                                              void *user)
{
    cfw_fuzz_wss_state_t *st = (cfw_fuzz_wss_state_t *)user;
    cfw_strview_t name;
    cfw_strview_t value;

    if (cfw_ws_handshake_validate(req) != CFW_OK ||
        cfw_ws_handshake_accept(req, st->accept,
                                sizeof(st->accept)) != CFW_OK) {
        res->status = CFW_HTTP_STATUS_BAD_REQUEST;
        return CFW_OK;
    }
    res->status = CFW_HTTP_STATUS_SWITCHING_PROTOCOLS;
    cfw_strview_from_cstr(&name, "Upgrade");
    cfw_strview_from_cstr(&value, "websocket");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Connection");
    cfw_strview_from_cstr(&value, "Upgrade");
    cfw_response_header_add(res, &name, &value);
    cfw_strview_from_cstr(&name, "Sec-WebSocket-Accept");
    cfw_strview_init(&value, st->accept, strlen(st->accept));
    cfw_response_header_add(res, &name, &value);
    (void)st;
    return CFW_OK;
}

static cfw_status_t cfw_fuzz_wss_hook(cfw_upgraded_conn_t *conn,
                                      uint32_t events, void *user)
{
    cfw_fuzz_wss_state_t *st = (cfw_fuzz_wss_state_t *)user;
    cfw_ws_t *ws;

    if (events == CFW_UPGRADE_EV_CLOSE) {
        ws = cfw_upgraded_user(conn);
        if (ws != NULL && ws == st->ws) {
            cfw_ws_destroy(ws);
            st->ws = NULL;
        }
        return CFW_OK;
    }
    if (events == CFW_UPGRADE_EV_READY) {
        cfw_ws_options_t options;
        memset(&options, 0, sizeof(options));
        options.max_message_size = 4096;
        ws = cfw_ws_create(conn, &options);
        if (ws == NULL) {
            return CFW_ERROR_INTERNAL;
        }
        cfw_upgraded_set_user(conn, ws);
        st->ws = ws;
        return CFW_OK;
    }
    ws = cfw_upgraded_user(conn);
    if (ws != NULL) {
        (void)cfw_ws_on_event(ws, events);
    }
    return CFW_OK;
}

static int cfw_fuzz_wss_server_main(void *arg)
{
    cfw_fuzz_wss_state_t *st = (cfw_fuzz_wss_state_t *)arg;
    return (int)cfw_server_run(&st->server);
}

/* Builds one masked client frame into out; returns bytes written or
 * 0 when cap is too small. payload_len may exceed plen (truncated
 * send is intentional). */
static size_t cfw_fuzz_wss_emit(uint8_t *out, size_t cap, uint8_t b0,
                                uint64_t declared, const uint8_t *mask,
                                const uint8_t *payload, size_t plen)
{
    size_t pos = 0;
    uint8_t b1;
    size_t i;

    if (declared < 126) {
        b1 = (uint8_t)(0x80u | declared);
    } else if (declared <= 0xFFFFu) {
        b1 = (uint8_t)(0x80u | 126u);
    } else {
        b1 = (uint8_t)(0x80u | 127u);
    }
    if (cap < 16) {
        return 0;
    }
    out[pos++] = b0;
    out[pos++] = b1;
    if (declared < 126) {
        /* length already in b1 */
    } else if (declared <= 0xFFFFu) {
        out[pos++] = (uint8_t)(declared >> 8);
        out[pos++] = (uint8_t)declared;
    } else {
        int shift;
        for (shift = 56; shift >= 0; shift -= 8) {
            out[pos++] = (uint8_t)(declared >> shift);
        }
    }
    if (pos + 4 + plen > cap) {
        return 0;
    }
    memcpy(out + pos, mask, 4);
    pos += 4;
    for (i = 0; i < plen; i++) {
        out[pos + i] = (uint8_t)(payload[i] ^ mask[i & 3u]);
    }
    pos += plen;
    return pos;
}

static int cfw_fuzz_wss_send_all(cfw_fd_t *fd, const uint8_t *data,
                                 size_t size)
{
    size_t off = 0;
    int attempts = 0;
    while (off < size && attempts < 20000) {
        size_t sent = 0;
        cfw_status_t st =
            cfw_net_send(fd, data + off, size - off, &sent);
        if (st == CFW_OK) {
            off += sent;
            attempts = 0;
            continue;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        cfw_test_sleep_us(200);
        attempts++;
    }
    return off == size ? 0 : 1;
}

static int cfw_fuzz_wss_recv_until(cfw_fd_t *fd, const char *needle,
                                   char *out, size_t cap, size_t *out_len)
{
    size_t len = 0;
    int attempts;
    out[0] = '\0';
    for (attempts = 0; attempts < 20000; attempts++) {
        size_t got = 0;
        cfw_status_t st;
        if (len + 1 < cap && strstr(out, needle) == NULL) {
            st = cfw_net_recv(fd, (unsigned char *)out + len,
                              cap - 1 - len, &got);
            if (st == CFW_OK && got > 0) {
                len += got;
                out[len] = '\0';
                if (strstr(out, needle) != NULL) {
                    *out_len = len;
                    return 0;
                }
                continue;
            }
            if (st == CFW_ERROR_IO) {
                break;
            }
        }
        cfw_test_sleep_us(200);
    }
    *out_len = len;
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char handshake[] =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    cfw_fuzz_wss_state_t st;
    cfw_thread_t thread;
    cfw_dispatch_options_t dopt;
    cfw_server_options_t sopt;
    cfw_strview_t pattern;
    cfw_fd_t client;
    cfw_addr_t addr;
    uint16_t port;
    uint64_t rng;
    size_t pos = 0;
    int frames = 0;
    char response[4096];
    size_t resp_len = 0;

    if (size == 0 || size > CFW_FUZZ_WSS_MAX_INPUT) {
        return 0;
    }

    memset(&st, 0, sizeof(st));
    st.arena = cfw_arena_create(65536);
    if (st.arena == NULL) {
        return 0;
    }
    if (cfw_router_init(&st.router, st.arena) != CFW_OK) {
        goto fail;
    }
    memset(&dopt, 0, sizeof(dopt));
    dopt.router = &st.router;
    dopt.handler = cfw_fuzz_wss_http_handler;
    dopt.handler_user = &st;
    dopt.max_middleware = 2;
    if (cfw_dispatch_init(&st.dispatch, st.arena, &dopt) != CFW_OK) {
        goto fail;
    }
    cfw_strview_from_cstr(&pattern, "/ws");
    if (cfw_dispatch_add_route(&st.dispatch, CFW_HTTP_METHOD_GET,
                               &pattern, cfw_fuzz_wss_http_handler,
                               &st) != CFW_OK) {
        goto fail;
    }
    memset(&sopt, 0, sizeof(sopt));
    sopt.host = "127.0.0.1";
    sopt.port = 0;
    sopt.backlog = 4;
    sopt.dispatch = &st.dispatch;
    sopt.http.max_head_size = 8192;
    sopt.http.max_body_size = 65536;
    sopt.http.max_pending_size = 65536;
    sopt.max_out_size = 65536;
    sopt.max_connections = 4;
    sopt.max_headers = 32;
    sopt.max_params = 16;
    sopt.upgrade = cfw_fuzz_wss_hook;
    sopt.upgrade_user = &st;
    if (cfw_server_init(&st.server, &sopt) != CFW_OK) {
        goto fail;
    }
    port = cfw_server_port(&st.server);
    if (port == 0) {
        goto fail_server;
    }
    if (cfw_thread_create(&thread, cfw_fuzz_wss_server_main, &st) !=
        CFW_OK) {
        goto fail_server;
    }

    /* Client: connect, handshake, stream structured frames. */
    cfw_fd_init(&client);
    {
        cfw_status_t cst = CFW_ERROR_IO;
        if (cfw_addr_from_str(&addr, "127.0.0.1", port) == CFW_OK &&
            cfw_net_socket(AF_INET, SOCK_STREAM, 0, &client) == CFW_OK) {
            cst = cfw_net_connect(&client, &addr);
        }
        if (cst != CFW_OK && cst != CFW_ERROR_WOULDBLOCK) {
            goto client_done;
        }
    }
    /* Loopback connect completes within a millisecond or two; give
     * the nonblocking connect time before the handshake send. */
    cfw_test_sleep_us(2000);
    if (cfw_fuzz_wss_send_all(&client, (const uint8_t *)handshake,
                              sizeof(handshake) - 1) != 0 ||
        cfw_fuzz_wss_recv_until(&client, "\r\n\r\n", response,
                                sizeof(response), &resp_len) != 0) {
        goto client_done;
    }

    /* Stream frames parsed from the input. */
    rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)size;
    while (pos + 2 <= size && frames < CFW_FUZZ_WSS_MAX_FRAMES) {
        uint8_t b0 = data[pos++];
        uint8_t lb = data[pos++];
        uint64_t declared;
        size_t avail;
        size_t plen;
        uint8_t mask[4];
        uint8_t frame[CFW_FUZZ_WSS_MAX_INPUT + 16];
        size_t flen;
        int shift;

        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        mask[0] = (uint8_t)rng;
        mask[1] = (uint8_t)(rng >> 8);
        mask[2] = (uint8_t)(rng >> 16);
        mask[3] = (uint8_t)(rng >> 24);

        if (lb < 126) {
            declared = lb;
        } else if (lb == 126) {
            if (pos + 2 > size) {
                break;
            }
            declared = ((uint64_t)data[pos] << 8) | data[pos + 1];
            pos += 2;
        } else {
            if (pos + 8 > size) {
                break;
            }
            declared = 0;
            for (shift = 0; shift < 8; shift++) {
                declared = (declared << 8) | data[pos + (size_t)shift];
            }
            pos += 8;
        }
        avail = size - pos;
        plen = avail < declared ? avail : (size_t)declared;
        if (plen > sizeof(frame) - 16) {
            plen = sizeof(frame) - 16;
        }
        flen = cfw_fuzz_wss_emit(frame, sizeof(frame), b0, declared, mask,
                                 data + pos, plen);
        if (flen == 0 ||
            cfw_fuzz_wss_send_all(&client, frame, flen) != 0) {
            break;
        }
        pos += plen;
        frames++;
        if (plen < declared) {
            break; /* truncated tail frame sent */
        }
    }

    /* Drain briefly so close-handshake frames reach the client. */
    {
        unsigned char drain[2048];
        int tries;
        for (tries = 0; tries < 5000; tries++) {
            size_t got = 0;
            cfw_status_t st =
                cfw_net_recv(&client, drain, sizeof(drain), &got);
            if (st == CFW_OK && got == 0) {
                break;
            }
            if (st == CFW_ERROR_IO) {
                break;
            }
            cfw_test_sleep_us(400);
        }
    }

client_done:
    cfw_fd_close(&client);
    cfw_server_stop(&st.server);
    {
        int exit_code = 0;
        (void)cfw_thread_join(&thread, &exit_code);
    }

fail_server:
    cfw_server_free(&st.server);
fail:
    cfw_arena_destroy(st.arena);
    return 0;
}
