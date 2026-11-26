/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static cfw_status_t cfw_test_feed_str(cfw_http1_server_t *server,
                                      const char *data)
{
    cfw_span_t span;
    cfw_span_init(&span, data, strlen(data));
    return cfw_http1_server_feed(server, &span);
}

/* Drains all currently available events into `events` (up to max). Returns
 * the count, or -1 on an unexpected status. */
static int cfw_test_drain(cfw_http1_server_t *server, cfw_http1_event_t *events,
                          int max)
{
    int count = 0;
    for (;;) {
        cfw_status_t status;
        if (count >= max) {
            return -1;
        }
        status = cfw_http1_server_next_event(server, &events[count]);
        if (status == CFW_ERROR_WOULDBLOCK) {
            return count;
        }
        if (status != CFW_OK) {
            return -1;
        }
        count++;
    }
}

static int test_http1_simple_get_keepalive(void)
{
    cfw_http1_server_options_t options = {4096, 0, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t events[8];
    int count;

    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, "GET / HTTP/1.1\r\n\r\n"),
                       CFW_OK);
    count = cfw_test_drain(&server, events, 8);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_EQ(events[0].kind, CFW_HTTP1_EV_HEAD);
    CFW_TEST_ASSERT_EQ(events[0].head.request_line.method,
                       CFW_HTTP_METHOD_GET);
    CFW_TEST_ASSERT_EQ(events[1].kind, CFW_HTTP1_EV_MESSAGE_DONE);
    /* Keep-alive: still open, waiting for the next request. */
    CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &events[0]),
                       CFW_ERROR_WOULDBLOCK);
    cfw_http1_server_free(&server);
    return 0;
}

static int test_http1_content_length_body(void)
{
    cfw_http1_server_options_t options = {4096, 0, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t events[8];
    static const char request[] =
        "POST /submit HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "hello world";
    char body[16];
    size_t body_len = 0;
    int saw_head = 0;
    int saw_done = 0;
    size_t i;
    int j;

    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    /* Feed one byte at a time. Event body spans are only valid until the
     * next feed, so copy bytes immediately during each drain. */
    for (i = 0; i < sizeof(request) - 1; i++) {
        cfw_span_t span;
        int n;
        cfw_span_init(&span, request + i, 1);
        CFW_TEST_ASSERT_EQ(cfw_http1_server_feed(&server, &span), CFW_OK);
        n = cfw_test_drain(&server, events, 8);
        CFW_TEST_ASSERT_TRUE(n >= 0);
        for (j = 0; j < n; j++) {
            if (events[j].kind == CFW_HTTP1_EV_HEAD) {
                CFW_TEST_ASSERT_EQ(events[j].head.request_line.method,
                                   CFW_HTTP_METHOD_POST);
                saw_head = 1;
            } else if (events[j].kind == CFW_HTTP1_EV_BODY) {
                CFW_TEST_ASSERT_TRUE(body_len + events[j].body.size <
                                     sizeof(body));
                memcpy(body + body_len, events[j].body.data,
                       events[j].body.size);
                body_len += events[j].body.size;
            } else if (events[j].kind == CFW_HTTP1_EV_MESSAGE_DONE) {
                saw_done = 1;
            }
        }
    }
    CFW_TEST_ASSERT_TRUE(saw_head);
    CFW_TEST_ASSERT_TRUE(saw_done);
    CFW_TEST_ASSERT_EQ(body_len, 11);
    CFW_TEST_ASSERT_EQ(memcmp(body, "hello world", 11), 0);
    cfw_http1_server_free(&server);
    return 0;
}

static int test_http1_chunked_body(void)
{
    cfw_http1_server_options_t options = {4096, 0, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t events[16];
    int count;
    static const char request[] =
        "POST /up HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, request), CFW_OK);
    count = cfw_test_drain(&server, events, 16);
    CFW_TEST_ASSERT_EQ(count, 4);
    CFW_TEST_ASSERT_EQ(events[0].kind, CFW_HTTP1_EV_HEAD);
    CFW_TEST_ASSERT_EQ(events[1].kind, CFW_HTTP1_EV_BODY);
    CFW_TEST_ASSERT_EQ(events[1].body.size, 4);
    CFW_TEST_ASSERT_EQ(memcmp(events[1].body.data, "Wiki", 4), 0);
    CFW_TEST_ASSERT_EQ(events[2].kind, CFW_HTTP1_EV_BODY);
    CFW_TEST_ASSERT_EQ(events[2].body.size, 5);
    CFW_TEST_ASSERT_EQ(memcmp(events[2].body.data, "pedia", 5), 0);
    CFW_TEST_ASSERT_EQ(events[3].kind, CFW_HTTP1_EV_MESSAGE_DONE);
    cfw_http1_server_free(&server);
    return 0;
}

static int test_http1_pipelined_requests(void)
{
    cfw_http1_server_options_t options = {4096, 0, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t events[16];
    int count;
    static const char two[] =
        "GET /a HTTP/1.1\r\n\r\n"
        "GET /b HTTP/1.1\r\nConnection: close\r\n\r\n";

    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, two), CFW_OK);
    count = cfw_test_drain(&server, events, 16);
    CFW_TEST_ASSERT_EQ(count, 4);
    CFW_TEST_ASSERT_EQ(events[0].kind, CFW_HTTP1_EV_HEAD);
    CFW_TEST_ASSERT_EQ(events[1].kind, CFW_HTTP1_EV_MESSAGE_DONE);
    CFW_TEST_ASSERT_EQ(events[2].kind, CFW_HTTP1_EV_HEAD);
    CFW_TEST_ASSERT_EQ(events[3].kind, CFW_HTTP1_EV_MESSAGE_DONE);
    /* Second request said Connection: close — connection is done. */
    CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &events[0]),
                       CFW_ERROR_WOULDBLOCK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, "GET /c HTTP/1.1\r\n\r\n"),
                       CFW_ERROR_WOULDBLOCK);
    cfw_http1_server_free(&server);
    return 0;
}

static int test_http1_connection_close_and_http10(void)
{
    cfw_http1_server_options_t options = {4096, 0, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t events[8];
    int count;

    /* HTTP/1.0 without keep-alive closes after one request. */
    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, "GET / HTTP/1.0\r\n\r\n"),
                       CFW_OK);
    count = cfw_test_drain(&server, events, 8);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &events[0]),
                       CFW_ERROR_WOULDBLOCK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, "GET / HTTP/1.0\r\n\r\n"),
                       CFW_ERROR_WOULDBLOCK);
    cfw_http1_server_free(&server);

    /* HTTP/1.0 with keep-alive stays open. */
    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(
                           &server,
                           "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
                       CFW_OK);
    count = cfw_test_drain(&server, events, 8);
    CFW_TEST_ASSERT_EQ(count, 2);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, "GET /2 HTTP/1.0\r\n\r\n"),
                       CFW_OK);
    count = cfw_test_drain(&server, events, 8);
    CFW_TEST_ASSERT_EQ(count, 2);
    cfw_http1_server_free(&server);
    return 0;
}

static int test_http1_protocol_errors(void)
{
    static const char *bad[] = {
        "GARBAGE\r\n\r\n",
        "GET / HTTP/1.1\r\nBad Header\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 5\r\n"
        "Content-Length: 6\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "Z\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nabcdXY0\r\n\r\n",
    };
    cfw_http1_server_options_t options = {4096, 0, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t event;
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int j;
        int saw_error = 0;
        CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_feed_str(&server, bad[i]), CFW_OK);
        /* A valid head may produce HEAD (and even BODY) events before the
         * malformed body is reached; the protocol error must surface. */
        for (j = 0; j < 6; j++) {
            CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &event),
                               CFW_OK);
            if (event.kind == CFW_HTTP1_EV_ERROR) {
                CFW_TEST_ASSERT_EQ(event.error_kind, CFW_HTTP1_ERR_PROTOCOL);
                saw_error = 1;
                break;
            }
        }
        CFW_TEST_ASSERT_TRUE(saw_error);
        /* Connection is dead after a protocol error. */
        CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &event),
                           CFW_ERROR_WOULDBLOCK);
        cfw_http1_server_free(&server);
    }
    return 0;
}

static int test_http1_resource_limits(void)
{
    cfw_http1_server_options_t options = {64, 5, 256};
    cfw_http1_server_t server;
    cfw_http1_event_t event;
    cfw_http1_event_t events[8];
    int count;

    /* Head exceeding max_head_size. */
    {
        char big[256];
        memset(big, 'A', sizeof(big));
        memcpy(big + sizeof(big) - 4, "\r\n\r\n", 4);
        CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
        {
            cfw_span_t span;
            cfw_span_init(&span, big, sizeof(big));
            /* First feed fills head buffer to limit. */
            CFW_TEST_ASSERT_EQ(cfw_http1_server_feed(&server, &span),
                               CFW_OK);
            /* Drain may produce nothing yet; keep feeding triggers error
             * on the next event attempt or feed. */
            count = cfw_test_drain(&server, events, 8);
            CFW_TEST_ASSERT_TRUE(count >= 0);
            /* Feed more: pending cap is fine but head reader now fails. */
            cfw_span_init(&span, "X", 1);
            CFW_TEST_ASSERT_EQ(cfw_http1_server_feed(&server, &span), CFW_OK);
            CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &event),
                               CFW_OK);
            CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_ERROR);
            CFW_TEST_ASSERT_EQ(event.error_kind, CFW_HTTP1_ERR_PROTOCOL);
        }
        cfw_http1_server_free(&server);
    }

    /* Content-Length above max_body_size → 413 before HEAD is emitted. */
    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(
                           &server,
                           "POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n"),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &event), CFW_OK);
    CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_ERROR);
    CFW_TEST_ASSERT_EQ(event.error_kind, CFW_HTTP1_ERR_BODY_TOO_LARGE);
    cfw_http1_server_free(&server);

    /* Pending buffer overflow → feed rejected. */
    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    {
        char blob[300];
        memset(blob, 'x', sizeof(blob));
        cfw_span_t span;
        cfw_span_init(&span, blob, sizeof(blob));
        CFW_TEST_ASSERT_EQ(cfw_http1_server_feed(&server, &span),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_http1_server_feed(&server, &span),
                           CFW_ERROR_WOULDBLOCK);
    }
    cfw_http1_server_free(&server);
    return 0;
}

static int test_http1_body_too_large_chunked(void)
{
    cfw_http1_server_options_t options = {4096, 4, 65536};
    cfw_http1_server_t server;
    cfw_http1_event_t event;
    int i;
    int saw_error = 0;

    CFW_TEST_ASSERT_EQ(cfw_http1_server_init(&server, &options), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_feed_str(
                           &server,
                           "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                           "\r\n5\r\nhello\r\n0\r\n\r\n"),
                       CFW_OK);
    /* HEAD first, then the 5-byte chunk exceeds max_body_size=4. */
    for (i = 0; i < 4; i++) {
        CFW_TEST_ASSERT_EQ(cfw_http1_server_next_event(&server, &event),
                           CFW_OK);
        if (event.kind == CFW_HTTP1_EV_ERROR) {
            CFW_TEST_ASSERT_EQ(event.error_kind, CFW_HTTP1_ERR_BODY_TOO_LARGE);
            saw_error = 1;
            break;
        }
    }
    CFW_TEST_ASSERT_TRUE(saw_error);
    cfw_http1_server_free(&server);
    return 0;
}

const cfw_test_case cfw_test_http1_cases[] = {
    {"http1_simple_get_keepalive", test_http1_simple_get_keepalive},
    {"http1_content_length_body", test_http1_content_length_body},
    {"http1_chunked_body", test_http1_chunked_body},
    {"http1_pipelined_requests", test_http1_pipelined_requests},
    {"http1_connection_close_and_http10", test_http1_connection_close_and_http10},
    {"http1_protocol_errors", test_http1_protocol_errors},
    {"http1_resource_limits", test_http1_resource_limits},
    {"http1_body_too_large_chunked", test_http1_body_too_large_chunked},
    {NULL, NULL},
};
