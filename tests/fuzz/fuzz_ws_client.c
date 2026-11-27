/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the WebSocket client frame parser
 * (ADR 0077): cfw_ws_client_connect performs a real handshake against
 * a fake server running on a cfw_thread over loopback TCP, then the
 * fuzz input is written verbatim as server-to-client bytes — masked
 * frames, control frames, fragmentation, oversized lengths, invalid
 * UTF-8, and close handshakes are all in scope.
 *
 * The server side is deliberately minimal: it accepts, extracts the
 * Sec-WebSocket-Key from the client handshake, replies 101 with the
 * correct accept hash, streams the input, and drains until the client
 * closes. Any protocol violation is an acceptable outcome; crashes
 * and sanitizer reports are the failure signal.
 */

#include "test_loopback.h"

#include "private/ws_proto.h"

#include <cfw/cfw.h>
#include <cfw/codec.h>
#include <cfw/ws_client.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CFW_FUZZ_WSC_MAX_INPUT (32u * 1024u)
#define CFW_FUZZ_WSC_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct {
    cfw_fd_t listener;
    const uint8_t *data;
    size_t size;
} cfw_fuzz_wsc_server_t;

static int cfw_fuzz_wsc_ascii_ieq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/* Extracts the trimmed value of the first "name:" header line found
 * case-insensitively within `head`. Returns 1 and sets *value/*vlen. */
static int cfw_fuzz_wsc_header(const char *head, size_t hlen,
                               const char *name, const char **value,
                               size_t *vlen)
{
    size_t nlen = strlen(name);
    size_t i;

    for (i = 0; i + nlen + 1 <= hlen; i++) {
        /* Only match at the start of a header line. */
        if (i != 0 && head[i - 1] != '\n') {
            continue;
        }
        if (cfw_fuzz_wsc_ascii_ieq(head + i, name, nlen) &&
            head[i + nlen] == ':') {
            const char *v = head + i + nlen + 1;
            const char *end = head + hlen;
            const char *line_end;
            while (v < end && (*v == ' ' || *v == '\t')) {
                v++;
            }
            line_end = v;
            while (line_end < end && *line_end != '\r' &&
                   *line_end != '\n') {
                line_end++;
            }
            while (line_end > v &&
                   (line_end[-1] == ' ' || line_end[-1] == '\t' ||
                    line_end[-1] == '\r')) {
                line_end--;
            }
            *value = v;
            *vlen = (size_t)(line_end - v);
            return 1;
        }
    }
    return 0;
}

static int cfw_fuzz_wsc_server_main(void *arg)
{
    cfw_fuzz_wsc_server_t *srv = (cfw_fuzz_wsc_server_t *)arg;
    cfw_fd_t conn;
    cfw_addr_t peer;
    unsigned char buf[16384];
    char head[16384];
    size_t hlen = 0;
    const char *key = NULL;
    size_t key_len = 0;
    cfw_ws_sha1_t sha;
    unsigned char digest[20];
    char accept_b64[32];
    char response[512];
    size_t rlen;
    size_t off;
    int attempts;

    cfw_fd_init(&conn);

    /* Accept with a bounded retry budget. */
    for (attempts = 0; attempts < 20000; attempts++) {
        cfw_status_t st = cfw_net_accept(&srv->listener, &peer, &conn);
        if (st == CFW_OK) {
            break;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
        cfw_test_sleep_us(200);
    }
    if (attempts == 20000) {
        return 1;
    }

    /* Read the client's HTTP handshake (bounded). */
    for (attempts = 0; attempts < 20000 && hlen < sizeof(head) - 1;
         attempts++) {
        size_t got = 0;
        cfw_status_t st =
            cfw_net_recv(&conn, buf, sizeof(buf), &got);
        if (st == CFW_OK && got > 0) {
            size_t copy = got;
            if (copy > sizeof(head) - 1 - hlen) {
                copy = sizeof(head) - 1 - hlen;
            }
            memcpy(head + hlen, buf, copy);
            hlen += copy;
            head[hlen] = '\0';
            if (strstr(head, "\r\n\r\n") != NULL) {
                break;
            }
            continue;
        }
        if (st == CFW_ERROR_IO) {
            break;
        }
        if (st != CFW_OK && st != CFW_ERROR_WOULDBLOCK) {
            break;
        }
        cfw_test_sleep_us(200);
    }

    if (!cfw_fuzz_wsc_header(head, hlen, "sec-websocket-key", &key,
                            &key_len) ||
        key_len == 0 || key_len > 128) {
        cfw_fd_close(&conn);
        return 1;
    }

    /* Sec-WebSocket-Accept = base64(SHA1(key + GUID)). */
    cfw_ws_sha1_init(&sha);
    cfw_ws_sha1_update(&sha, (const uint8_t *)key, key_len);
    cfw_ws_sha1_update(&sha, (const uint8_t *)CFW_FUZZ_WSC_GUID,
                       sizeof(CFW_FUZZ_WSC_GUID) - 1);
    cfw_ws_sha1_final(&sha, digest);
    if (cfw_base64_encode(digest, sizeof(digest), accept_b64,
                          sizeof(accept_b64), NULL) != CFW_OK) {
        cfw_fd_close(&conn);
        return 1;
    }

    rlen = (size_t)snprintf(response, sizeof(response),
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n\r\n",
                            accept_b64);
    off = 0;
    while (off < rlen) {
        size_t sent = 0;
        cfw_status_t st =
            cfw_net_send(&conn, response + off, rlen - off, &sent);
        if (st == CFW_OK) {
            off += sent;
            continue;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            cfw_fd_close(&conn);
            return 1;
        }
        cfw_test_sleep_us(200);
    }

    /* Stream the fuzz input as server-to-client bytes. */
    off = 0;
    while (off < srv->size) {
        size_t want = srv->size - off;
        size_t sent = 0;
        cfw_status_t st;
        if (want > 1024) {
            want = 1024;
        }
        st = cfw_net_send(&conn, srv->data + off, want, &sent);
        if (st == CFW_OK) {
            off += sent;
            continue;
        }
        if (st != CFW_ERROR_WOULDBLOCK) {
            break;
        }
        cfw_test_sleep_us(200);
    }

    /* Drain until the client closes or the budget ends. */
    for (attempts = 0; attempts < 20000; attempts++) {
        size_t got = 0;
        cfw_status_t st = cfw_net_recv(&conn, buf, sizeof(buf), &got);
        if (st == CFW_OK && got == 0) {
            break; /* orderly close */
        }
        if (st == CFW_ERROR_IO) {
            break;
        }
        cfw_test_sleep_us(500);
    }

    cfw_fd_close(&conn);
    return 0;
}

static cfw_status_t cfw_fuzz_wsc_on_message(cfw_ws_client_t *ws,
                                            const cfw_ws_message_t *msg,
                                            void *user)
{
    size_t *total = (size_t *)user;
    (void)ws;
    *total += msg->payload.size;
    return CFW_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cfw_fuzz_wsc_server_t srv;
    cfw_thread_t thread;
    cfw_ws_client_options_t opts;
    cfw_ws_client_t *ws = NULL;
    cfw_addr_t bind_addr;
    cfw_addr_t local_addr;
    uint16_t port = 0;
    cfw_duration_t timeout;
    size_t total = 0;
    int pump_iters;

    if (size == 0 || size > CFW_FUZZ_WSC_MAX_INPUT) {
        return 0;
    }
    if (cfw_addr_any(0, 0, &bind_addr) != CFW_OK ||
        cfw_net_socket(AF_INET, SOCK_STREAM, 0, &srv.listener) != CFW_OK ||
        cfw_net_bind(&srv.listener, &bind_addr) != CFW_OK ||
        cfw_net_listen(&srv.listener, 4) != CFW_OK ||
        cfw_net_local_addr(&srv.listener, &local_addr) != CFW_OK ||
        cfw_addr_port(&local_addr, &port) != CFW_OK) {
        cfw_fd_close(&srv.listener);
        return 0;
    }

    srv.data = data;
    srv.size = size;
    if (cfw_thread_create(&thread, cfw_fuzz_wsc_server_main, &srv) !=
        CFW_OK) {
        cfw_fd_close(&srv.listener);
        return 0;
    }

    memset(&opts, 0, sizeof(opts));
    opts.host = "127.0.0.1";
    opts.port = port;
    opts.path = "/fuzz";
    opts.on_message = cfw_fuzz_wsc_on_message;
    opts.user = &total;
    opts.max_message_size = 1u << 20;
    if (cfw_duration_from_millis(3000, &timeout) == CFW_OK) {
        opts.timeout = timeout;
    }

    if (cfw_ws_client_connect(&opts, &ws) == CFW_OK && ws != NULL) {
        cfw_duration_t slice;
        if (cfw_duration_from_millis(1, &slice) == CFW_OK) {
            for (pump_iters = 0; pump_iters < 20000 &&
                               !cfw_ws_client_is_closed(ws);
                 pump_iters++) {
                cfw_status_t st = cfw_ws_client_pump(ws, &slice);
                if (st != CFW_OK && st != CFW_ERROR_TIMEOUT) {
                    break;
                }
            }
        }
        cfw_ws_client_destroy(ws);
    }

    {
        int exit_code = 0;
        (void)cfw_thread_join(&thread, &exit_code);
    }
    cfw_fd_close(&srv.listener);
    return 0;
}
