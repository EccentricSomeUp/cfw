/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

/*
 * Standalone driver for the fuzz targets (ADR 0077). Used when the
 * toolchain lacks libFuzzer (GCC, MSVC, Windows): each target binary
 * is built with this main() instead of -fsanitize=fuzzer, and replays
 * input files through LLVMFuzzerTestOneInput.
 *
 * Usage: cfw_fuzz_<target> <file> [<file> ...]
 *        cfw_fuzz_<target>            (reads stdin)
 *        cfw_fuzz_<target> -         (reads stdin)
 *
 * Inputs are capped at 1 MiB; larger files are truncated.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        } else if (cfw_fuzz_driver_file(argv[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}
