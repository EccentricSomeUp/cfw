/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/thread.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if CFW_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <pthread.h>
#endif

#if CFW_PLATFORM_POSIX
    _Static_assert(sizeof(pthread_t) <= sizeof(((cfw_thread_t *)NULL)->u.storage),
                   "cfw_thread_t storage is too small for pthread_t");
#endif

/*
 * Both backends need a single pointer-sized parameter block to carry
 * (fn, arg) into the platform trampoline; heap-allocating it keeps the
 * public cfw_thread_t storage a plain handle.
 */
typedef struct cfw_thread_start {
    cfw_thread_fn fn;
    void *arg;
} cfw_thread_start_t;

#if CFW_PLATFORM_WINDOWS
static DWORD WINAPI cfw_thread_trampoline(LPVOID parameter)
{
    cfw_thread_start_t *start = (cfw_thread_start_t *)parameter;
    int result = start->fn(start->arg);
    free(start);
    return (DWORD)result;
}
#else
static void *cfw_thread_trampoline(void *parameter)
{
    cfw_thread_start_t *start = (cfw_thread_start_t *)parameter;
    int result = start->fn(start->arg);
    free(start);
    return (void *)(intptr_t)result;
}
#endif

static cfw_status_t cfw_thread_store(cfw_thread_t *thread,
                                     const void *handle, size_t handle_size)
{
    memset(thread->u.storage, 0, sizeof(thread->u.storage));
    memcpy(thread->u.storage, handle, handle_size);
    return CFW_OK;
}

cfw_status_t cfw_thread_create(cfw_thread_t *thread, cfw_thread_fn fn,
                               void *arg)
{
    cfw_thread_start_t *start;

    if (thread == NULL || fn == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    start = (cfw_thread_start_t *)malloc(sizeof(*start));
    if (start == NULL) {
        return CFW_ERROR_NOMEM;
    }
    start->fn = fn;
    start->arg = arg;

#if CFW_PLATFORM_WINDOWS
    {
        HANDLE handle = CreateThread(NULL, 0, cfw_thread_trampoline, start, 0,
                                     NULL);
        if (handle == NULL) {
            free(start);
            return CFW_ERROR_UNAVAILABLE;
        }
        return cfw_thread_store(thread, &handle, sizeof(handle));
    }
#else
    {
        pthread_t handle;
        int rc = pthread_create(&handle, NULL, cfw_thread_trampoline, start);
        if (rc != 0) {
            free(start);
            return rc == ENOMEM ? CFW_ERROR_NOMEM : CFW_ERROR_UNAVAILABLE;
        }
        return cfw_thread_store(thread, &handle, sizeof(handle));
    }
#endif
}

cfw_status_t cfw_thread_join(cfw_thread_t *thread, int *exit_code)
{
    if (thread == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_PLATFORM_WINDOWS
    {
        HANDLE handle;
        DWORD code = 0;
        memcpy(&handle, thread->u.storage, sizeof(handle));
        if (handle == NULL) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0) {
            return CFW_ERROR_INTERNAL;
        }
        if (!GetExitCodeThread(handle, &code)) {
            CloseHandle(handle);
            return CFW_ERROR_INTERNAL;
        }
        CloseHandle(handle);
        if (exit_code != NULL) {
            *exit_code = (int)code;
        }
        return CFW_OK;
    }
#else
    {
        pthread_t handle;
        void *raw_result = NULL;
        int rc;
        memcpy(&handle, thread->u.storage, sizeof(handle));
        rc = pthread_join(handle, &raw_result);
        if (rc != 0) {
            return CFW_ERROR_INTERNAL;
        }
        if (exit_code != NULL) {
            *exit_code = (int)(intptr_t)raw_result;
        }
        return CFW_OK;
    }
#endif
}

cfw_status_t cfw_thread_detach(cfw_thread_t *thread)
{
    if (thread == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_PLATFORM_WINDOWS
    {
        HANDLE handle;
        memcpy(&handle, thread->u.storage, sizeof(handle));
        if (handle == NULL) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        /* Closing our reference detaches: the thread keeps running and
         * the kernel reclaims its resources at exit. */
        CloseHandle(handle);
        return CFW_OK;
    }
#else
    {
        pthread_t handle;
        memcpy(&handle, thread->u.storage, sizeof(handle));
        if (pthread_detach(handle) != 0) {
            return CFW_ERROR_INTERNAL;
        }
        return CFW_OK;
    }
#endif
}
