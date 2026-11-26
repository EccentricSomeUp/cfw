/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/fs.h>

#if defined(_WIN32)
    #include <sys/stat.h>
    #define CFW_STAT_STRUCT struct _stat64
    #define CFW_STAT_FUNC _stat64
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #define CFW_STAT_STRUCT struct stat
    #define CFW_STAT_FUNC stat
#endif

cfw_status_t cfw_file_size(const char *path, uint64_t *size)
{
    CFW_STAT_STRUCT st;

    if (path == NULL || size == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (CFW_STAT_FUNC(path, &st) != 0) {
        return CFW_ERROR_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *size = (uint64_t)st.st_size;
    return CFW_OK;
}

cfw_status_t cfw_file_is_directory(const char *path, int *is_dir)
{
    CFW_STAT_STRUCT st;

    if (path == NULL || is_dir == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (CFW_STAT_FUNC(path, &st) != 0) {
        return CFW_ERROR_IO;
    }

    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return CFW_OK;
}
