/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for the HTTP client response path
 * (ADR 0077): a fake server on a cfw_thread accepts the client's
 * request, then writes the fuzz input verbatim as raw response bytes.
 * cfw_http_client_request parses status line, headers, framing
 * (Content-Length / chunked / close-delimited), and the decoded body
 * — status codes, ambiguous framing, chunked extensions/trailers,
 * oversized lengths, and truncation are all in scope.
 */

#include "test_loopback.h"

#include <cfw/cfw.h>
#include <cfw/http_client.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CFW_FUZZ_HTTPC_MAX_INPUT (64u * 1024u)

typedef struct {
    cfw_fd_t listener;
    const uint8_t *data;
    size_t size;
} cfw_fuzz_htc_server_t;

static int cfw_fuzz_htc_server_main(void *arg)
{
    cfw_fuzz_htc_server_t *srv = (cfw_fuzz_htc_server_t *)arg;
    cfw_fd_t conn;
    cfw_addr_t peer;
    unsigned char buf[8192];
    size_t off = 0;
    int attempts;

    cfw_fd_init(&conn);
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

    /* Consume the request head so the client finishes writing before
     * the response stream begins. */
    {
        char head[8192];
        size_t hlen = 0;
        head[0] = '\0';
        for (attempts = 0; attempts < 20000 &&
                            strstr(head, "\r\n\r\n") == NULL &&
                            hlen < sizeof(head) - 1;
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
                continue;
            }
            if (st == CFW_ERROR_IO) {
                break;
            }
            cfw_test_sleep_us(200);
        }
    }

    /* Stream the fuzz input as the raw HTTP response. */
    while (off < srv->size) {
        size_t want = srv->size - off;
        size_t sent = 0;
        cfw_status_t st;
        if (want > 4096) {
            want = 4096;
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

    /* Close right after the response stream: the request head was
     * fully consumed, so the close is a graceful FIN — the client
     * sees EOF after the response bytes (close-delimited bodies
     * included) instead of deadlocking against our drain wait. */
    cfw_fd_close(&conn);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cfw_fuzz_htc_server_t srv;
    cfw_thread_t thread;
    cfw_http_client_request_t req;
    cfw_http_client_response_t response;
    cfw_addr_t bind_addr;
    cfw_addr_t local_addr;
    cfw_duration_t timeout;
    uint16_t port = 0;

    if (size == 0 || size > CFW_FUZZ_HTTPC_MAX_INPUT) {
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
    if (cfw_thread_create(&thread, cfw_fuzz_htc_server_main, &srv) !=
        CFW_OK) {
        cfw_fd_close(&srv.listener);
        return 0;
    }

    memset(&req, 0, sizeof(req));
    req.host = "127.0.0.1";
    req.port = port;
    req.method = CFW_HTTP_METHOD_GET;
    req.target = "/fuzz";
    req.max_head_size = 16384;
    req.max_body_size = 1u << 20;
    if (cfw_duration_from_millis(5000, &timeout) == CFW_OK) {
        req.timeout = timeout;
    }

    memset(&response, 0, sizeof(response));
    (void)cfw_http_client_request(&req, &response);
    {
        cfw_http_response_head_t head;
        (void)cfw_http_client_response_head(&response, &head);
    }
    cfw_http_client_response_free(&response);

    /* The client has disconnected by now, so the fake server's drain
     * loop sees EOF and exits; join it. */
    {
        int exit_code = 0;
        (void)cfw_thread_join(&thread, &exit_code);
    }
    cfw_fd_close(&srv.listener);
    return 0;
}
