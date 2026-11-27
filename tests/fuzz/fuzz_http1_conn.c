/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Coverage-guided fuzz target for cfw_http1_conn over a real socket
 * pair (ADR 0077): unlike fuzz_http's in-memory cfw_http1_server
 * pipeline, this drives the connection object — incremental reads,
 * event dispatch, response queueing, flush backpressure, keep-alive
 * pipelining, and close handling — against a TCP loopback peer whose
 * bytes are the fuzz input.
 *
 * Input is fed incrementally inside the pump loop so arbitrarily
 * large inputs cannot deadlock the bounded socket buffers.
 */

#include "test_loopback.h"

#include <cfw/cfw.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CFW_FUZZ_CONN_MAX_INPUT (64u * 1024u)
#define CFW_FUZZ_CONN_MAX_ITERS 60000

static const char cfw_fuzz_conn_response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cfw_fd_t pair[2];
    cfw_http1_conn_t conn;
    cfw_http1_conn_options_t options;
    size_t written = 0;
    int closed = 0;
    int idle = 0;
    int iters;

    if (size == 0 || size > CFW_FUZZ_CONN_MAX_INPUT ||
        cfw_test_socketpair(pair) != CFW_OK) {
        return 0;
    }

    memset(&options, 0, sizeof(options));
    options.http.max_head_size = 8192;
    options.http.max_body_size = 65536;
    options.http.max_pending_size = 1u << 20;
    options.max_out_size = 8192;
    if (cfw_http1_conn_init(&conn, &pair[0], &options) != CFW_OK) {
        cfw_fd_close(&pair[0]);
        cfw_fd_close(&pair[1]);
        return 0;
    }

    for (iters = 0; iters < CFW_FUZZ_CONN_MAX_ITERS && !closed; iters++) {
        cfw_http1_event_t event;
        unsigned char drain[2048];
        size_t got = 0;
        cfw_status_t st;

        /* Feed the peer a slice of remaining input. */
        if (written < size) {
            idle = 0;
            size_t want = size - written;
            size_t chunk = 0;
            if (want > 512) {
                want = 512;
            }
            st = cfw_net_send(&pair[1], data + written, want, &chunk);
            if (st == CFW_OK) {
                written += chunk;
            } else if (st != CFW_ERROR_WOULDBLOCK) {
                break;
            }
        }

        /* Drain whatever the conn wrote so its send buffer never
         * fills and stalls the pump. */
        for (;;) {
            st = cfw_net_recv(&pair[1], drain, sizeof(drain), &got);
            if (st != CFW_OK || got == 0) {
                break;
            }
        }

        st = cfw_http1_conn_next_event(&conn, &event);
        if (st == CFW_ERROR_WOULDBLOCK) {
            (void)cfw_http1_conn_flush(&conn);
            /* Once all input is fed and the conn is idle, further
             * iterations can never make progress — the peer holds the
             * connection open but sends nothing more. */
            if (written >= size && ++idle > 500) {
                break;
            }
            continue;
        }
        idle = 0;
        if (st != CFW_OK) {
            break;
        }

        switch (event.kind) {
        case CFW_HTTP1_EV_MESSAGE_DONE:
            (void)cfw_http1_conn_queue(&conn, cfw_fuzz_conn_response,
                                       sizeof(cfw_fuzz_conn_response) - 1);
            (void)cfw_http1_conn_flush(&conn);
            break;
        case CFW_HTTP1_EV_ERROR:
            closed = 1;
            break;
        default:
            break;
        }
        if (cfw_http1_conn_is_closed(&conn)) {
            closed = 1;
        }
    }

    /* conn owns pair[0] (released at init); conn_free closes it. */
    cfw_http1_conn_free(&conn);
    cfw_fd_close(&pair[1]);
    return 0;
}
