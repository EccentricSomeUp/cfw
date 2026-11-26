/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)
    #include "private/net.h"

    #include <errno.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <unistd.h>

/* Creates a connected loopback TCP pair; *server_out receives the accepted
 * side. Both fds are nonblocking cfw_fd_t owners. */
static int cfw_test_tcp_pair(cfw_fd_t *server_out, cfw_fd_t *client_out)
{
    cfw_fd_t listener;
    cfw_fd_t client;
    cfw_fd_t server_side;
    cfw_sockaddr_t bind_addr;
    cfw_sockaddr_t connect_addr;
    cfw_sockaddr_t local_addr;
    cfw_sockaddr_t peer_addr;
    uint16_t port;
    int attempts;

    cfw_fd_init(&listener);
    cfw_fd_init(&client);
    cfw_fd_init(&server_side);

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &listener),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_bind(&listener, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_listen(&listener, 4), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_local_addr(&listener, &local_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&local_addr, &port), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&connect_addr, "127.0.0.1", port),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &client),
                       CFW_OK);

    {
        cfw_status_t status = cfw_net_connect(&client, &connect_addr);
        if (status != CFW_OK && status != CFW_ERROR_WOULDBLOCK) {
            return 1;
        }
    }

    /* Nonblocking connect completes asynchronously; retry accept. */
    for (attempts = 0; attempts < 1000; attempts++) {
        cfw_status_t status =
            cfw_net_accept(&listener, &peer_addr, &server_side);
        if (status == CFW_OK) {
            break;
        }
        if (status != CFW_ERROR_WOULDBLOCK) {
            cfw_fd_close(&listener);
            cfw_fd_close(&client);
            return 1;
        }
        usleep(1000);
    }
    if (attempts == 1000) {
        cfw_fd_close(&listener);
        cfw_fd_close(&client);
        return 1;
    }

    cfw_fd_close(&listener);
    *server_out = server_side;
    *client_out = client;
    return 0;
}

static int cfw_test_client_send(cfw_fd_t *client, const char *data)
{
    ssize_t n = send((int)cfw_fd_get(client), data, strlen(data), 0);
    return n == (ssize_t)strlen(data) ? 0 : 1;
}

/* Reads up to `capacity` bytes from the client with a short retry loop for
 * nonblocking delivery. Returns bytes read, 0 on EOF, -1 on timeout. */
static ssize_t cfw_test_client_recv(cfw_fd_t *client, char *buffer,
                                    size_t capacity)
{
    int attempts;
    for (attempts = 0; attempts < 200; attempts++) {
        ssize_t n = recv((int)cfw_fd_get(client), buffer, capacity, 0);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        usleep(1000);
    }
    return -1;
}

static cfw_http1_conn_options_t cfw_test_conn_options(void)
{
    cfw_http1_conn_options_t options;
    options.http.max_head_size = 4096;
    options.http.max_body_size = 0;
    options.http.max_pending_size = 65536;
    options.max_out_size = 4096;
    return options;
}

static int test_http1_conn_request_response(void)
{
    cfw_fd_t server_fd;
    cfw_fd_t client_fd;
    cfw_http1_conn_t conn;
    cfw_http1_conn_options_t options = cfw_test_conn_options();
    cfw_http1_event_t event;
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    char buffer[256];
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_tcp_pair(&server_fd, &client_fd), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, &server_fd, &options),
                       CFW_OK);
    CFW_TEST_ASSERT_FALSE(cfw_fd_valid(&server_fd));

    /* No data yet: WOULDBLOCK. */
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_next_event(&conn, &event),
                       CFW_ERROR_WOULDBLOCK);

    CFW_TEST_ASSERT_EQ(cfw_test_client_send(&client_fd, "GET / HTTP/1.1\r\n\r\n"), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_next_event(&conn, &event), CFW_OK);
    CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_HEAD);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_next_event(&conn, &event), CFW_OK);
    CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_MESSAGE_DONE);
    CFW_TEST_ASSERT_FALSE(cfw_http1_conn_is_closed(&conn));

    /* Queue and flush a response; the client receives it. */
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_queue(&conn, response,
                                             sizeof(response) - 1),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_http1_conn_pending_out(&conn) > 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_flush(&conn), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_pending_out(&conn), 0);
    CFW_TEST_ASSERT_FALSE(cfw_http1_conn_is_closed(&conn));

    n = cfw_test_client_recv(&client_fd, buffer, sizeof(buffer));
    CFW_TEST_ASSERT_TRUE(n == (ssize_t)(sizeof(response) - 1));
    CFW_TEST_ASSERT_EQ(memcmp(buffer, response, (size_t)n), 0);

    cfw_http1_conn_free(&conn);
    cfw_fd_close(&client_fd);
    return 0;
}

static int test_http1_conn_close_after_response(void)
{
    cfw_fd_t server_fd;
    cfw_fd_t client_fd;
    cfw_http1_conn_t conn;
    cfw_http1_conn_options_t options = cfw_test_conn_options();
    cfw_http1_event_t event;
    static const char response[] = "HTTP/1.1 200 OK\r\n\r\n";
    char buffer[256];
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_tcp_pair(&server_fd, &client_fd), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, &server_fd, &options),
                       CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_client_send(&client_fd, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_next_event(&conn, &event), CFW_OK);
    CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_HEAD);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_next_event(&conn, &event), CFW_OK);
    CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_MESSAGE_DONE);

    CFW_TEST_ASSERT_EQ(cfw_http1_conn_queue(&conn, response,
                                             sizeof(response) - 1),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_flush(&conn), CFW_OK);
    /* close-after-write drained the queue: socket is closed. */
    CFW_TEST_ASSERT_TRUE(cfw_http1_conn_is_closed(&conn));

    n = cfw_test_client_recv(&client_fd, buffer, sizeof(buffer));
    CFW_TEST_ASSERT_TRUE(n == (ssize_t)(sizeof(response) - 1));
    CFW_TEST_ASSERT_EQ(memcmp(buffer, response, (size_t)n), 0);
    /* Subsequent recv sees EOF. */
    n = cfw_test_client_recv(&client_fd, buffer, sizeof(buffer));
    CFW_TEST_ASSERT_EQ(n, 0);

    cfw_http1_conn_free(&conn);
    cfw_fd_close(&client_fd);
    return 0;
}

static int test_http1_conn_protocol_error(void)
{
    cfw_fd_t server_fd;
    cfw_fd_t client_fd;
    cfw_http1_conn_t conn;
    cfw_http1_conn_options_t options = cfw_test_conn_options();
    cfw_http1_event_t event;
    static const char response[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
    char buffer[256];
    ssize_t n;

    CFW_TEST_ASSERT_EQ(cfw_test_tcp_pair(&server_fd, &client_fd), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, &server_fd, &options),
                       CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_client_send(&client_fd, "GARBAGE\r\n\r\n"), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_next_event(&conn, &event), CFW_OK);
    CFW_TEST_ASSERT_EQ(event.kind, CFW_HTTP1_EV_ERROR);
    CFW_TEST_ASSERT_EQ(event.error_kind, CFW_HTTP1_ERR_PROTOCOL);

    /* Error latches close-after-write; respond then flush closes. */
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_queue(&conn, response,
                                             sizeof(response) - 1),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_flush(&conn), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_http1_conn_is_closed(&conn));

    n = cfw_test_client_recv(&client_fd, buffer, sizeof(buffer));
    CFW_TEST_ASSERT_TRUE(n == (ssize_t)(sizeof(response) - 1));
    CFW_TEST_ASSERT_EQ(memcmp(buffer, response, (size_t)n), 0);

    cfw_http1_conn_free(&conn);
    cfw_fd_close(&client_fd);
    return 0;
}

static int test_http1_conn_queue_limits(void)
{
    cfw_fd_t server_fd;
    cfw_fd_t client_fd;
    cfw_http1_conn_t conn;
    cfw_http1_conn_options_t options = cfw_test_conn_options();
    char blob[8192];

    memset(blob, 'x', sizeof(blob));
    options.max_out_size = 1024;

    CFW_TEST_ASSERT_EQ(cfw_test_tcp_pair(&server_fd, &client_fd), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, &server_fd, &options),
                       CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_http1_conn_queue(&conn, blob, 512), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_pending_out(&conn), 512);
    /* Exceeding the cap is rejected without consuming. */
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_queue(&conn, blob, 1024),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_pending_out(&conn), 512);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_queue(&conn, blob, 512), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_pending_out(&conn), 1024);

    cfw_http1_conn_free(&conn);
    cfw_fd_close(&client_fd);
    return 0;
}

static int test_http1_conn_init_validation(void)
{
    cfw_fd_t server_fd;
    cfw_fd_t client_fd;
    cfw_http1_conn_t conn;
    cfw_http1_conn_options_t options = cfw_test_conn_options();

    CFW_TEST_ASSERT_EQ(cfw_test_tcp_pair(&server_fd, &client_fd), 0);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(NULL, &server_fd, &options),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, NULL, &options),
                       CFW_ERROR_INVALID_ARGUMENT);
    options.max_out_size = 0;
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, &server_fd, &options),
                       CFW_ERROR_INVALID_ARGUMENT);
    options = cfw_test_conn_options();
    CFW_TEST_ASSERT_EQ(cfw_http1_conn_init(&conn, &server_fd, &options),
                       CFW_OK);
    cfw_http1_conn_free(&conn);
    cfw_fd_close(&client_fd);
    return 0;
}

const cfw_test_case cfw_test_http1_conn_cases[] = {
    {"http1_conn_request_response", test_http1_conn_request_response},
    {"http1_conn_close_after_response", test_http1_conn_close_after_response},
    {"http1_conn_protocol_error", test_http1_conn_protocol_error},
    {"http1_conn_queue_limits", test_http1_conn_queue_limits},
    {"http1_conn_init_validation", test_http1_conn_init_validation},
    {NULL, NULL},
};

#else

const cfw_test_case cfw_test_http1_conn_cases[] = {
    {NULL, NULL},
};

#endif
