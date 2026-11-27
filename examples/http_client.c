/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Minimal cfw HTTP client: performs one GET request and prints the
 * status code and decoded body. Usage:
 *
 *   cfw_example_http_client <host> <port> <target>
 *
 * e.g. cfw_example_http_client 127.0.0.1 8080 /hello
 *
 * TLS is not wired up here; see include/cfw/http_client.h for the
 * cfw_tls_ctx_t hook (client role, verify_peer + CA file).
 */

#include <cfw/cfw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    cfw_http_client_request_t req;
    cfw_http_client_response_t response;
    cfw_http_response_head_t head;
    cfw_status_t status;
    unsigned long port;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <host> <port> <target>\n", argv[0]);
        return 2;
    }
    port = strtoul(argv[2], NULL, 10);
    if (port == 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[2]);
        return 2;
    }

    memset(&req, 0, sizeof(req));
    req.host = argv[1];
    req.port = (uint16_t)port;
    req.method = CFW_HTTP_METHOD_GET;
    req.target = argv[3];
    cfw_duration_from_millis(10000, &req.timeout);

    memset(&response, 0, sizeof(response));
    status = cfw_http_client_request(&req, &response);
    if (status != CFW_OK) {
        fprintf(stderr, "request failed: %s\n", cfw_status_string(status));
        cfw_http_client_response_free(&response);
        return 1;
    }

    if (cfw_http_client_response_head(&response, &head) == CFW_OK) {
        printf("HTTP/%u.%u %u\n", head.version.major, head.version.minor,
               head.status_code);
    }
    printf("%.*s\n", (int)response.body.size,
           (const char *)response.body.data);

    cfw_http_client_response_free(&response);
    return 0;
}
