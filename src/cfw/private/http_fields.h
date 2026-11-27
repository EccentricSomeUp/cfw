/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_HTTP_FIELDS_H
#define CFW_PRIVATE_HTTP_FIELDS_H

/*
 * Internal HTTP field grammar helpers shared by the wire formatters
 * in src/cfw/http.c and protocol clients that emit header blocks
 * without going through cfw_http_*_head_format (e.g. the WebSocket
 * client's Upgrade request). Not part of the public API.
 */

#include <cfw/strview.h>

#include <stddef.h>

int cfw_http_is_tchar(unsigned char c);
int cfw_http_field_name_valid(const cfw_strview_t *name);
int cfw_http_field_value_valid(const char *data, size_t size);

#endif /* CFW_PRIVATE_HTTP_FIELDS_H */
