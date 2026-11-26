/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#if !defined(_WIN32)
    #include "private/net.h"

    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <string.h>
    #include <unistd.h>

static int test_addr_from_str_and_back(void)
{
    cfw_sockaddr_t addr;
    char text[INET6_ADDRSTRLEN];
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "127.0.0.1", 8080), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv4(&addr));
    CFW_TEST_ASSERT_FALSE(cfw_addr_is_ipv6(&addr));
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&addr, &port), CFW_OK);
    CFW_TEST_ASSERT_EQ(port, 8080);
    CFW_TEST_ASSERT_EQ(cfw_addr_to_str(&addr, text, sizeof(text)), CFW_OK);
    CFW_TEST_ASSERT_STR_EQ(text, "127.0.0.1");

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "::1", 443), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv6(&addr));
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&addr, &port), CFW_OK);
    CFW_TEST_ASSERT_EQ(port, 443);
    CFW_TEST_ASSERT_EQ(cfw_addr_to_str(&addr, text, sizeof(text)), CFW_OK);
    CFW_TEST_ASSERT_STR_EQ(text, "::1");

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "not-an-ip", 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&addr, "999.1.1.1", 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(NULL, "1.2.3.4", 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_addr_to_str(NULL, text, sizeof(text)),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_addr_any(void)
{
    cfw_sockaddr_t addr;
    uint16_t port;

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &addr), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv4(&addr));
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&addr, &port), CFW_OK);
    CFW_TEST_ASSERT_EQ(port, 0);

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 1, &addr), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv6(&addr));

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_tcp_loopback_connect_accept(void)
{
    cfw_fd_t listener;
    cfw_fd_t client;
    cfw_fd_t server_side;
    cfw_sockaddr_t bind_addr;
    cfw_sockaddr_t local_addr;
    cfw_sockaddr_t peer_addr;
    cfw_sockaddr_t connect_addr;
    uint16_t port;
    char byte = 'q';
    char received = 0;
    cfw_status_t status;

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &listener),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_bind(&listener, &bind_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_listen(&listener, 4), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_local_addr(&listener, &local_addr), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_addr_port(&local_addr, &port), CFW_OK);
    CFW_TEST_ASSERT(port != 0);

    CFW_TEST_ASSERT_EQ(cfw_addr_from_str(&connect_addr, "127.0.0.1", port),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(AF_INET, SOCK_STREAM, 0, &client),
                       CFW_OK);

    status = cfw_net_connect(&client, &connect_addr);
    /* Nonblocking connect may complete immediately or report in-progress. */
    CFW_TEST_ASSERT(status == CFW_OK || status == CFW_ERROR_WOULDBLOCK ||
                    status == CFW_ERROR_IO);

    /* Nonblocking connect may still be in progress; accept retries until
     * the loopback handshake completes. */
    {
        int tries;
        status = CFW_ERROR_WOULDBLOCK;
        for (tries = 0; tries < 1000 && status == CFW_ERROR_WOULDBLOCK;
             tries++) {
            status = cfw_net_accept(&listener, &peer_addr, &server_side);
            if (status == CFW_ERROR_WOULDBLOCK) {
                usleep(1000);
            }
        }
        CFW_TEST_ASSERT_EQ(status, CFW_OK);
    }
    CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv4(&peer_addr));

    /* Give the connection a moment, then exchange a byte. */
    {
        int tries;
        for (tries = 0; tries < 100; tries++) {
            if (send((int)client.handle, &byte, 1, 0) == 1) {
                break;
            }
        }
        CFW_TEST_ASSERT(tries < 100);
    }
    {
        int tries;
        for (tries = 0; tries < 100; tries++) {
            if (recv((int)server_side.handle, &received, 1, 0) == 1) {
                break;
            }
        }
        CFW_TEST_ASSERT(tries < 100);
        CFW_TEST_ASSERT_EQ(received, 'q');
    }

    CFW_TEST_ASSERT_EQ(cfw_fd_close(&client), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&server_side), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&listener), CFW_OK);
    return 0;
}

static int test_net_argument_validation(void)
{
    cfw_fd_t fd;
    cfw_sockaddr_t addr;

    CFW_TEST_ASSERT_EQ(cfw_addr_any(0, 0, &addr), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_net_socket(0, 0, 0, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_net_socket(-1, SOCK_STREAM, 0, &fd),
                       CFW_ERROR_IO);

    cfw_fd_init(&fd);
    CFW_TEST_ASSERT_EQ(cfw_net_bind(&fd, &addr), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_net_set_nonblocking(&fd, 1),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_net_listen(&fd, -1), CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}
#endif

const cfw_test_case cfw_test_net_cases[] = {
#if !defined(_WIN32)
    {"addr_from_str_and_back", test_addr_from_str_and_back},
    {"addr_any", test_addr_any},
    {"tcp_loopback_connect_accept", test_tcp_loopback_connect_accept},
    {"net_argument_validation", test_net_argument_validation},
#endif
    {NULL, NULL},
};
