/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_NET_RESOLVE_H
#define CFW_PRIVATE_NET_RESOLVE_H

/*
 * Internal multi-address resolver. Not part of the public API: the
 * public cfw_addr_resolve keeps its first-result contract, while
 * connection setup (e.g. the HTTP client) needs every getaddrinfo
 * result in a family so connect can fall back across multiple A/AAAA
 * records instead of relying on resolver ordering.
 */

#include <cfw/net.h>
#include <cfw/status.h>

#include <stddef.h>

typedef struct cfw_addr_list {
    /* malloc'd array of `count` addresses; owned by the caller. */
    cfw_addr_t *items;
    size_t count;
} cfw_addr_list_t;

/* BLOCKING like cfw_addr_resolve: intended for connection setup,
 * never inside an event loop. On success `list` receives a malloc'd
 * array of every usable address in `family`, in resolver order;
 * release it with cfw_addr_list_free. Returns
 * CFW_ERROR_INVALID_ARGUMENT for unresolvable names or bad arguments,
 * CFW_ERROR_IO when no usable address is returned,
 * CFW_ERROR_NOMEM on allocation failure. */
cfw_status_t cfw_addr_resolve_all(cfw_addr_list_t *list,
                                  const char *host, uint16_t port,
                                  cfw_addr_family_t family);

/* Releases a list from cfw_addr_resolve_all. NULL-safe. */
void cfw_addr_list_free(cfw_addr_list_t *list);

#endif /* CFW_PRIVATE_NET_RESOLVE_H */
