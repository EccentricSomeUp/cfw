/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Nonblocking TCP echo server built entirely on the public socket,
 * pollset, and fd APIs (cfw/net.h + cfw/pollset.h + cfw/fd.h).
 *
 * Demonstrates the canonical pattern: one pollset waits on the
 * listener plus every client; each client buffers unsent echo bytes
 * and toggles CFW_POLL_OUT interest only while its pending queue is
 * non-empty (level-triggered semantics make unconditional OUT
 * interest a busy loop).
 *
 * Usage: cfw_example_tcp_echo [port]   (default 9000)
 * Then:   nc 127.0.0.1 9000
 */

#include <cfw/cfw.h>

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define CFW_ECHO_MAX_CLIENTS 64
#define CFW_ECHO_PENDING_CAP 8192
#define CFW_ECHO_READ_SIZE 4096

typedef struct {
    cfw_fd_t fd;
    /* Echo bytes received but not yet fully sent. */
    unsigned char pending[CFW_ECHO_PENDING_CAP];
    size_t pending_len;
} cfw_echo_client_t;

static cfw_echo_client_t cfw_echo_clients[CFW_ECHO_MAX_CLIENTS];

/* Token encodes the client slot index + 1; NULL token = listener. */
static void *cfw_echo_token(size_t index)
{
    return (void *)(intptr_t)(index + 1);
}

static cfw_echo_client_t *cfw_echo_client_from_token(void *token)
{
    intptr_t index = (intptr_t)token - 1;
    if (index < 0 || (size_t)index >= CFW_ECHO_MAX_CLIENTS) {
        return NULL;
    }
    return &cfw_echo_clients[index];
}

/* Registers IN always, OUT only while bytes are pending. */
static cfw_status_t cfw_echo_client_interest(cfw_pollset_t *pollset,
                                             cfw_echo_client_t *client)
{
    uint32_t events = CFW_POLL_IN;
    if (client->pending_len != 0) {
        events |= CFW_POLL_OUT;
    }
    return cfw_pollset_modify(pollset, cfw_fd_get(&client->fd), events,
                              cfw_echo_token(
                                  (size_t)(client - cfw_echo_clients)));
}

/* Sends as much of the pending queue as the socket accepts. */
static cfw_status_t cfw_echo_flush(cfw_pollset_t *pollset,
                                   cfw_echo_client_t *client)
{
    while (client->pending_len != 0) {
        size_t sent = 0;
        cfw_status_t status =
            cfw_net_send(&client->fd, client->pending,
                         client->pending_len, &sent);
        if (status == CFW_ERROR_WOULDBLOCK) {
            return cfw_echo_client_interest(pollset, client);
        }
        if (status != CFW_OK) {
            return status;
        }
        if (sent == 0) {
            return CFW_ERROR_IO;
        }
        memmove(client->pending, client->pending + sent,
                client->pending_len - sent);
        client->pending_len -= sent;
    }
    return cfw_echo_client_interest(pollset, client);
}

static void cfw_echo_client_close(cfw_pollset_t *pollset, size_t index)
{
    cfw_echo_client_t *client = &cfw_echo_clients[index];
    (void)cfw_pollset_remove(pollset, cfw_fd_get(&client->fd));
    cfw_fd_close(&client->fd);
    cfw_fd_init(&client->fd);
    client->pending_len = 0;
}

static cfw_status_t cfw_echo_accept_all(cfw_pollset_t *pollset,
                                        cfw_fd_t *listener)
{
    for (;;) {
        cfw_addr_t peer;
        cfw_fd_t conn;
        size_t slot;
        cfw_status_t status;

        cfw_fd_init(&conn);
        status = cfw_net_accept(listener, &peer, &conn);
        if (status == CFW_ERROR_WOULDBLOCK) {
            return CFW_OK;
        }
        if (status != CFW_OK) {
            return status;
        }
        {
            char peer_ip[48];
            uint16_t peer_port = 0;
            if (cfw_addr_to_str(&peer, peer_ip, sizeof(peer_ip)) == CFW_OK &&
                cfw_addr_port(&peer, &peer_port) == CFW_OK) {
                fprintf(stderr, "cfw_echo: accepted %s:%u\n", peer_ip,
                        (unsigned)peer_port);
            }
        }

        /* Find a free slot. */
        for (slot = 0; slot < CFW_ECHO_MAX_CLIENTS; slot++) {
            if (!cfw_fd_valid(&cfw_echo_clients[slot].fd)) {
                break;
            }
        }
        if (slot == CFW_ECHO_MAX_CLIENTS) {
            fprintf(stderr, "cfw_echo: client limit reached; "
                            "closing connection\n");
            cfw_fd_close(&conn);
            continue;
        }

        cfw_echo_clients[slot].fd = conn;
        cfw_echo_clients[slot].pending_len = 0;
        status = cfw_pollset_add(pollset, cfw_fd_get(&conn), CFW_POLL_IN,
                                 cfw_echo_token(slot));
        if (status != CFW_OK) {
            cfw_fd_close(&conn);
            cfw_fd_init(&cfw_echo_clients[slot].fd);
            return status;
        }
    }
}

static cfw_status_t cfw_echo_client_ready(cfw_pollset_t *pollset,
                                          cfw_echo_client_t *client,
                                          uint32_t events)
{
    unsigned char buffer[CFW_ECHO_READ_SIZE];

    if ((events & (CFW_POLL_ERR | CFW_POLL_HUP)) != 0) {
        return CFW_ERROR_IO;
    }

    /* Flush pending echo bytes first so a slow reader cannot grow the
     * queue unboundedly. */
    if (client->pending_len != 0) {
        cfw_status_t status = cfw_echo_flush(pollset, client);
        if (status != CFW_OK || client->pending_len != 0) {
            return status;
        }
    }

    if ((events & CFW_POLL_IN) == 0) {
        return CFW_OK;
    }

    for (;;) {
        size_t got = 0;
        cfw_status_t status =
            cfw_net_recv(&client->fd, buffer, sizeof(buffer), &got);
        if (status == CFW_ERROR_WOULDBLOCK) {
            return CFW_OK;
        }
        if (status != CFW_OK) {
            return status;
        }
        if (got == 0) {
            /* Orderly peer shutdown. */
            return CFW_ERROR_IO;
        }
        if (client->pending_len + got > CFW_ECHO_PENDING_CAP) {
            return CFW_ERROR_OVERFLOW;
        }
        memcpy(client->pending + client->pending_len, buffer, got);
        client->pending_len += got;
        {
            cfw_status_t flush_status =
                cfw_echo_flush(pollset, client);
            if (flush_status != CFW_OK) {
                return flush_status;
            }
        }
        /* Level-triggered: keep draining until WOULDBLOCK. */
        if (got < sizeof(buffer)) {
            return CFW_OK;
        }
    }
}

int main(int argc, char **argv)
{
    uint16_t port = 9000;
    cfw_pollset_t *pollset = NULL;
    cfw_fd_t listener;
    cfw_addr_t bind_addr;
    cfw_addr_t local_addr;
    cfw_poll_event_t events[CFW_ECHO_MAX_CLIENTS + 1];
    size_t slot;

    if (argc > 1) {
        long parsed = strtol(argv[1], NULL, 10);
        if (parsed <= 0 || parsed > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 2;
        }
        port = (uint16_t)parsed;
    }

    for (slot = 0; slot < CFW_ECHO_MAX_CLIENTS; slot++) {
        cfw_fd_init(&cfw_echo_clients[slot].fd);
        cfw_echo_clients[slot].pending_len = 0;
    }

    if (cfw_pollset_create(&pollset) != CFW_OK) {
        fprintf(stderr, "cfw_pollset_create failed "
                        "(is a poll backend available?)\n");
        return 1;
    }

    cfw_fd_init(&listener);
    if (cfw_addr_any(port, 0, &bind_addr) != CFW_OK ||
        cfw_net_socket(AF_INET, SOCK_STREAM, 0, &listener) != CFW_OK ||
        cfw_net_bind(&listener, &bind_addr) != CFW_OK ||
        cfw_net_listen(&listener, CFW_ECHO_MAX_CLIENTS) != CFW_OK) {
        fprintf(stderr, "failed to bind/listen on port %u\n",
                (unsigned)port);
        cfw_fd_close(&listener);
        cfw_pollset_destroy(pollset);
        return 1;
    }
    if (cfw_net_local_addr(&listener, &local_addr) == CFW_OK) {
        uint16_t bound = 0;
        cfw_addr_port(&local_addr, &bound);
        port = bound;
    }
    if (cfw_pollset_add(pollset, cfw_fd_get(&listener), CFW_POLL_IN,
                        NULL) != CFW_OK) {
        fprintf(stderr, "cfw_pollset_add(listener) failed\n");
        cfw_fd_close(&listener);
        cfw_pollset_destroy(pollset);
        return 1;
    }

    fprintf(stderr, "cfw tcp_echo listening on 127.0.0.1:%u\n",
            (unsigned)port);

    for (;;) {
        size_t count = 0;
        size_t i;
        cfw_status_t status =
            cfw_pollset_wait(pollset, NULL, events,
                             CFW_ECHO_MAX_CLIENTS + 1, &count);
        if (status == CFW_ERROR_TIMEOUT) {
            continue;
        }
        if (status != CFW_OK) {
            fprintf(stderr, "cfw_pollset_wait failed: %s\n",
                    cfw_status_string(status));
            break;
        }
        for (i = 0; i < count; i++) {
            if (events[i].handle == cfw_fd_get(&listener)) {
                status = cfw_echo_accept_all(pollset, &listener);
                if (status != CFW_OK) {
                    fprintf(stderr, "accept failed: %s\n",
                            cfw_status_string(status));
                }
                continue;
            }
            {
                cfw_echo_client_t *client =
                    cfw_echo_client_from_token(events[i].token);
                if (client == NULL ||
                    cfw_fd_get(&client->fd) != events[i].handle) {
                    continue; /* stale event for a closed client */
                }
                status = cfw_echo_client_ready(pollset, client,
                                               events[i].events);
                if (status != CFW_OK) {
                    size_t index =
                        (size_t)(client - cfw_echo_clients);
                    cfw_echo_client_close(pollset, index);
                }
            }
        }
    }

    for (slot = 0; slot < CFW_ECHO_MAX_CLIENTS; slot++) {
        if (cfw_fd_valid(&cfw_echo_clients[slot].fd)) {
            cfw_fd_close(&cfw_echo_clients[slot].fd);
        }
    }
    cfw_fd_close(&listener);
    cfw_pollset_destroy(pollset);
    return 0;
}
