/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_WINSOCK_H
#define CFW_PRIVATE_WINSOCK_H

/*
 * One-time Winsock2 initialization shared by the Windows socket and
 * pollset backends (ADR 0072). Each translation unit that includes this
 * header gets its own INIT_ONCE; WSAStartup itself is process-global
 * and refcounted, and WSACleanup is intentionally never called — the
 * library does not own process lifetime.
 */

#if CFW_PLATFORM_WINDOWS

    #include <winsock2.h>

    #include <cfw/status.h>

static INIT_ONCE cfw_wsa_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK cfw_wsa_init_cb(PINIT_ONCE once, PVOID param,
                                     PVOID *context)
{
    WSADATA data;
    (void)once;
    (void)param;
    (void)context;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static cfw_status_t cfw_wsa_ensure(void)
{
    if (!InitOnceExecuteOnce(&cfw_wsa_once, cfw_wsa_init_cb, NULL, NULL)) {
        return CFW_ERROR_UNAVAILABLE;
    }
    return CFW_OK;
}

#endif /* CFW_PLATFORM_WINDOWS */

#endif /* CFW_PRIVATE_WINSOCK_H */
