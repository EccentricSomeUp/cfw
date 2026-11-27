/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Standalone driver for the fuzz targets (ADR 0077). Used when the
 * toolchain lacks libFuzzer (GCC, MSVC, Windows): each target binary
 * is built with this main() instead of -fsanitize=fuzzer, and replays
 * input files through LLVMFuzzerTestOneInput.
 *
 * Usage: cfw_fuzz_<target> <file|dir> [<file|dir> ...]
 *        cfw_fuzz_<target>                     (reads stdin)
 *        cfw_fuzz_<target> -                   (reads stdin)
 *
 * Directory arguments replay every regular file they contain in
 * sorted order (non-recursive). Passing the corpus directory avoids
 * argv-length limits on platforms with restricted command lines.
 *
 * Inputs are capped at 1 MiB; larger files are truncated.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define CFW_FUZZ_DRIVER_MAX_INPUT (1u << 20)

static int cfw_fuzz_driver_run(const uint8_t *data, size_t size)
{
    if (size > CFW_FUZZ_DRIVER_MAX_INPUT) {
        size = CFW_FUZZ_DRIVER_MAX_INPUT;
    }
    LLVMFuzzerTestOneInput(data, size);
    return 0;
}

static int cfw_fuzz_driver_file(const char *path)
{
    FILE *file;
    uint8_t *buf;
    long size;
    int rc;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cfw_fuzz: cannot open %s\n", path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        fprintf(stderr, "cfw_fuzz: cannot size %s\n", path);
        return 1;
    }
    buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(file);
        fprintf(stderr, "cfw_fuzz: out of memory\n");
        return 1;
    }
    if (fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        fprintf(stderr, "cfw_fuzz: cannot read %s\n", path);
        return 1;
    }
    fclose(file);
    rc = cfw_fuzz_driver_run(buf, (size_t)size);
    free(buf);
    return rc;
}

struct cfw_fuzz_namelist {
    char **items;
    size_t count;
    size_t cap;
};

static int cfw_fuzz_namelist_add(struct cfw_fuzz_namelist *list,
                                 const char *name)
{
    char *copy;

    if (list->count == list->cap) {
        size_t ncap = list->cap != 0 ? list->cap * 2 : 64;
        char **nitems =
            (char **)realloc(list->items, ncap * sizeof(*nitems));
        if (nitems == NULL) {
            return -1;
        }
        list->items = nitems;
        list->cap = ncap;
    }
    copy = (char *)malloc(strlen(name) + 1);
    if (copy == NULL) {
        return -1;
    }
    strcpy(copy, name);
    list->items[list->count++] = copy;
    return 0;
}

static int cfw_fuzz_name_cmp(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static void cfw_fuzz_namelist_free(struct cfw_fuzz_namelist *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int cfw_fuzz_is_dir(const char *path)
{
#ifdef _WIN32
    struct _stat st;

    if (_stat(path, &st) != 0) {
        return 0;
    }
    return (st.st_mode & _S_IFMT) == _S_IFDIR;
#else
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
#endif
}

static int cfw_fuzz_driver_dir(const char *path)
{
    struct cfw_fuzz_namelist list;
    size_t i;
    int rc = 0;

    memset(&list, 0, sizeof(list));

#ifdef _WIN32
    {
        char *pattern;
        struct _finddata_t fd;
        intptr_t handle;
        size_t plen = strlen(path);

        pattern = (char *)malloc(plen + 3);
        if (pattern == NULL) {
            return 1;
        }
        memcpy(pattern, path, plen);
        pattern[plen] = '\\';
        pattern[plen + 1] = '*';
        pattern[plen + 2] = '\0';
        handle = _findfirst(pattern, &fd);
        free(pattern);
        if (handle == -1) {
            fprintf(stderr, "cfw_fuzz: cannot list %s\n", path);
            return 1;
        }
        do {
            if (strcmp(fd.name, ".") == 0 ||
                strcmp(fd.name, "..") == 0) {
                continue;
            }
            if (cfw_fuzz_namelist_add(&list, fd.name) != 0) {
                rc = 1;
                break;
            }
        } while (_findnext(handle, &fd) == 0);
        _findclose(handle);
    }
#else
    {
        DIR *dir = opendir(path);
        struct dirent *ent;

        if (dir == NULL) {
            fprintf(stderr, "cfw_fuzz: cannot list %s\n", path);
            return 1;
        }
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            if (cfw_fuzz_namelist_add(&list, ent->d_name) != 0) {
                rc = 1;
                break;
            }
        }
        closedir(dir);
    }
#endif

    if (rc != 0) {
        cfw_fuzz_namelist_free(&list);
        return 1;
    }
    qsort(list.items, list.count, sizeof(*list.items),
          cfw_fuzz_name_cmp);
    for (i = 0; i < list.count; i++) {
        size_t plen = strlen(path);
        size_t nlen = strlen(list.items[i]);
        char *full = (char *)malloc(plen + nlen + 2);

        if (full == NULL) {
            cfw_fuzz_namelist_free(&list);
            return 1;
        }
        memcpy(full, path, plen);
        full[plen] = '/';
        memcpy(full + plen + 1, list.items[i], nlen + 1);
        if (cfw_fuzz_driver_file(full) != 0) {
            rc = 1;
        }
        free(full);
    }
    cfw_fuzz_namelist_free(&list);
    return rc;
}

static int cfw_fuzz_driver_stdin(void)
{
    uint8_t *buf = (uint8_t *)malloc(CFW_FUZZ_DRIVER_MAX_INPUT);
    size_t got = 0;
    size_t n;

    if (buf == NULL) {
        return 1;
    }
    while (got < CFW_FUZZ_DRIVER_MAX_INPUT &&
           (n = fread(buf + got, 1,
                      CFW_FUZZ_DRIVER_MAX_INPUT - got, stdin)) > 0) {
        got += n;
    }
    cfw_fuzz_driver_run(buf, got);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    int rc = 0;

    if (argc <= 1) {
        return cfw_fuzz_driver_stdin();
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (cfw_fuzz_driver_stdin() != 0) {
                rc = 1;
            }
        } else if (cfw_fuzz_is_dir(argv[i])) {
            if (cfw_fuzz_driver_dir(argv[i]) != 0) {
                rc = 1;
            }
        } else if (cfw_fuzz_driver_file(argv[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}
