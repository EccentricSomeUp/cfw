/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <cfw/net.h>

#include "private/net_resolve.h"
#include "test_loopback.h"

#if CFW_PLATFORM_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <string.h>
    #include <unistd.h>
#endif

static int test_addr_from_str_and_back(void)
{
    cfw_addr_t addr;
    char text[48];
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
    cfw_addr_t addr;
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
    cfw_addr_t bind_addr;
    cfw_addr_t local_addr;
    cfw_addr_t peer_addr;
    cfw_addr_t connect_addr;
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
                cfw_test_sleep_us(1000);
            }
        }
        CFW_TEST_ASSERT_EQ(status, CFW_OK);
    }
    CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv4(&peer_addr));

    /* Give the connection a moment, then exchange a byte through the
     * public wrappers (nonblocking: retry on WOULDBLOCK). */
    {
        int tries;
        size_t one = 0;
        for (tries = 0; tries < 100 && one == 0; tries++) {
            cfw_status_t st = cfw_net_send(&client, &byte, 1, &one);
            CFW_TEST_ASSERT(st == CFW_OK ||
                            st == CFW_ERROR_WOULDBLOCK);
            if (one == 0) {
                cfw_test_sleep_us(1000);
            }
        }
        CFW_TEST_ASSERT(tries < 100);
        CFW_TEST_ASSERT_EQ(one, 1);
    }
    {
        int tries;
        size_t got = 0;
        for (tries = 0; tries < 100 && got == 0; tries++) {
            cfw_status_t st =
                cfw_net_recv(&server_side, &received, 1, &got);
            CFW_TEST_ASSERT(st == CFW_OK ||
                            st == CFW_ERROR_WOULDBLOCK);
            if (got == 0) {
                cfw_test_sleep_us(1000);
            }
        }
        CFW_TEST_ASSERT(tries < 100);
        CFW_TEST_ASSERT_EQ(got, 1);
        CFW_TEST_ASSERT_EQ(received, 'q');
    }

    {
        /* Public send/recv wrappers deliver the same byte. */
        size_t sent = 0;
        size_t received2 = 0;
        char byte2 = 'z';
        char got2 = 0;
        CFW_TEST_ASSERT_EQ(cfw_net_send(&client, &byte2, 1, &sent),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(sent, 1);
        {
            int tries;
            cfw_status_t rst;
            for (tries = 0; tries < 100 && received2 == 0; tries++) {
                rst = cfw_net_recv(&server_side, &got2, 1, &received2);
                CFW_TEST_ASSERT(rst == CFW_OK ||
                                rst == CFW_ERROR_WOULDBLOCK);
                if (received2 == 0) {
                    cfw_test_sleep_us(1000);
                }
            }
        }
        CFW_TEST_ASSERT_EQ(received2, 1);
        CFW_TEST_ASSERT_EQ(got2, 'z');
    }

    {
        /* Peer address is visible on both ends. */
        cfw_addr_t client_peer;
        uint16_t peer_port = 0;
        CFW_TEST_ASSERT_EQ(cfw_net_peer_addr(&server_side, &client_peer),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_addr_port(&client_peer, &peer_port),
                           CFW_OK);
        CFW_TEST_ASSERT(peer_port != 0);
        CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv4(&client_peer));

        /* SO_ERROR reports a healthy socket. */
        {
            int sock_err = -1;
            CFW_TEST_ASSERT_EQ(cfw_net_so_error(&client, &sock_err),
                               CFW_OK);
            CFW_TEST_ASSERT_EQ(sock_err, 0);
        }
    }

    CFW_TEST_ASSERT_EQ(cfw_fd_close(&client), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&server_side), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_fd_close(&listener), CFW_OK);
    return 0;
}

static int test_net_argument_validation(void)
{
    cfw_fd_t fd;
    cfw_addr_t addr;

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

static int test_addr_resolve_all(void)
{
    cfw_addr_list_t list;
    cfw_addr_t single;
    uint16_t port;
    size_t i;

    /* Argument validation needs no resolver. */
    CFW_TEST_ASSERT_EQ(cfw_addr_resolve_all(NULL, "localhost", 80,
                                             CFW_ADDR_FAMILY_IPV4),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_addr_resolve_all(&list, "", 80,
                                             CFW_ADDR_FAMILY_IPV4),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_addr_resolve_all(&list, "localhost", 80,
                                             (cfw_addr_family_t)99),
                       CFW_ERROR_INVALID_ARGUMENT);
    cfw_addr_list_free(NULL);

    /* "localhost" must resolve on any host with a working resolver;
     * skip gracefully in minimal containers without one. */
    if (cfw_addr_resolve_all(&list, "localhost", 80,
                             CFW_ADDR_FAMILY_IPV4) != CFW_OK) {
        return 0;
    }
    CFW_TEST_ASSERT_TRUE(list.count >= 1);
    CFW_TEST_ASSERT_TRUE(list.items != NULL);
    for (i = 0; i < list.count; i++) {
        CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv4(&list.items[i]));
        CFW_TEST_ASSERT_EQ(cfw_addr_port(&list.items[i], &port), CFW_OK);
        CFW_TEST_ASSERT_EQ(port, 80);
    }
    /* The first enumerated address matches the public
     * single-address resolver. */
    if (cfw_addr_resolve(&single, "localhost", 80,
                         CFW_ADDR_FAMILY_IPV4) == CFW_OK) {
        CFW_TEST_ASSERT_EQ(list.items[0].length, single.length);
        CFW_TEST_ASSERT_EQ(memcmp(list.items[0].storage.opaque,
                                  single.storage.opaque,
                                  single.length),
                           0);
    }
    cfw_addr_list_free(&list);
    CFW_TEST_ASSERT_TRUE(list.items == NULL);
    CFW_TEST_ASSERT_EQ(list.count, 0);

    /* The IPv6 family returns only IPv6 addresses when it resolves. */
    if (cfw_addr_resolve_all(&list, "localhost", 80,
                             CFW_ADDR_FAMILY_IPV6) == CFW_OK) {
        CFW_TEST_ASSERT_TRUE(list.count >= 1);
        for (i = 0; i < list.count; i++) {
            CFW_TEST_ASSERT_TRUE(cfw_addr_is_ipv6(&list.items[i]));
        }
        cfw_addr_list_free(&list);
    }
    return 0;
}

const cfw_test_case cfw_test_net_cases[] = {
    {"addr_from_str_and_back", test_addr_from_str_and_back},
    {"addr_any", test_addr_any},
    {"addr_resolve_all", test_addr_resolve_all},
    {"tcp_loopback_connect_accept", test_tcp_loopback_connect_accept},
    {"net_argument_validation", test_net_argument_validation},
    {NULL, NULL},
};
