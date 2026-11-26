/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "private/fs_confined.h"
#include "test_harness.h"

#include <stdio.h>
#include <string.h>

static int test_fs_file_size_success(void)
{
    const char *path = "cfw_test_fs_tmp.bin";
    FILE *file;
    uint64_t size;
    const char payload[] = "cfw-fs-test-payload";

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    CFW_TEST_ASSERT_EQ(cfw_file_size(path, &size), CFW_OK);
    CFW_TEST_ASSERT_EQ(size, sizeof(payload) - 1);

    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}

static int test_fs_file_size_errors(void)
{
    uint64_t size;

    CFW_TEST_ASSERT_EQ(cfw_file_size(NULL, &size),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_size("definitely/not/here", &size),
                       CFW_ERROR_IO);

    /* A directory is not a regular file. */
    CFW_TEST_ASSERT_EQ(cfw_file_size(".", &size),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_fs_is_directory(void)
{
    int is_dir;

    CFW_TEST_ASSERT_EQ(cfw_file_is_directory(NULL, &is_dir),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_is_directory(".", NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_is_directory("definitely/not/here", &is_dir),
                       CFW_ERROR_IO);

    CFW_TEST_ASSERT_EQ(cfw_file_is_directory(".", &is_dir), CFW_OK);
    CFW_TEST_ASSERT_TRUE(is_dir);

    {
        const char *path = "cfw_test_fs_tmp.txt";
        FILE *file = fopen(path, "wb");
        CFW_TEST_ASSERT(file != NULL);
        CFW_TEST_ASSERT_EQ(fclose(file), 0);

        CFW_TEST_ASSERT_EQ(cfw_file_is_directory(path, &is_dir), CFW_OK);
        CFW_TEST_ASSERT_FALSE(is_dir);
        CFW_TEST_ASSERT_EQ(remove(path), 0);
    }
    return 0;
}

static int test_fs_file_open_read_close(void)
{
    const char *path = "cfw_test_fs_read.bin";
    const char payload[] = "cfw-fs-read-payload-0123456789";
    FILE *file;
    cfw_file_t fh;
    unsigned char buf[8];
    size_t got;
    size_t off = 0;

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    cfw_file_init(&fh);
    CFW_TEST_ASSERT_FALSE(cfw_file_valid(&fh));
    CFW_TEST_ASSERT_EQ(cfw_file_open(&fh, path), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_file_valid(&fh));

    /* Read in small chunks until EOF. */
    for (;;) {
        CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                           CFW_OK);
        if (got == 0) {
            break;
        }
        CFW_TEST_ASSERT_TRUE(off + got <= sizeof(payload) - 1);
        CFW_TEST_ASSERT_EQ(memcmp(buf, payload + off, got), 0);
        off += got;
    }
    CFW_TEST_ASSERT_EQ(off, sizeof(payload) - 1);

    /* Reads at EOF keep returning 0. */
    CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(got, 0);

    /* Close is idempotent. */
    CFW_TEST_ASSERT_EQ(cfw_file_close(&fh), CFW_OK);
    CFW_TEST_ASSERT_FALSE(cfw_file_valid(&fh));
    CFW_TEST_ASSERT_EQ(cfw_file_close(&fh), CFW_OK);

    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}

static int test_fs_file_open_errors(void)
{
    cfw_file_t fh;
    unsigned char buf[8];
    size_t got;

    cfw_file_init(&fh);
    CFW_TEST_ASSERT_EQ(cfw_file_open(NULL, "x"),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open(&fh, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open(&fh, "definitely/not/here"),
                       CFW_ERROR_IO);
    CFW_TEST_ASSERT_FALSE(cfw_file_valid(&fh));

    /* Read/close on an invalid handle. */
    CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_read(NULL, buf, sizeof(buf), &got),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, 0, &got),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_close(&fh), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_file_close(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Opening an already-open file is rejected (prevents handle leak). */
    {
        const char *path = "cfw_test_fs_open.bin";
        FILE *file = fopen(path, "wb");
        CFW_TEST_ASSERT(file != NULL);
        CFW_TEST_ASSERT_EQ(fclose(file), 0);

        CFW_TEST_ASSERT_EQ(cfw_file_open(&fh, path), CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_file_open(&fh, path),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_file_close(&fh), CFW_OK);
        CFW_TEST_ASSERT_EQ(remove(path), 0);
    }
    return 0;
}

static int test_fs_file_stream(void)
{
    const char *path = "cfw_test_fs_stream.bin";
    const char payload[] = "cfw-fs-stream-payload-0123456789";
    FILE *file;
    cfw_file_stream_t stream;
    unsigned char buf[7];
    cfw_span_t chunk;
    int eof;
    size_t off = 0;
    int pulls = 0;

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    /* Init errors. */
    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(NULL, path, buf,
                                            sizeof(buf)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&stream, NULL, buf,
                                            sizeof(buf)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&stream, path, NULL,
                                            sizeof(buf)),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&stream, path, buf, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&stream,
                                            "definitely/not/here", buf,
                                            sizeof(buf)),
                       CFW_ERROR_IO);

    CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&stream, path, buf,
                                            sizeof(buf)),
                       CFW_OK);

    /* Pull chunks until eof; verify reassembled content. */
    for (;;) {
        CFW_TEST_ASSERT_EQ(cfw_file_stream_read(&stream, &chunk, &eof),
                           CFW_OK);
        pulls++;
        if (eof) {
            CFW_TEST_ASSERT_EQ(chunk.size, 0);
            break;
        }
        CFW_TEST_ASSERT_TRUE(chunk.size <= sizeof(buf));
        CFW_TEST_ASSERT_TRUE(chunk.data == buf);
        CFW_TEST_ASSERT_TRUE(off + chunk.size <= sizeof(payload) - 1);
        CFW_TEST_ASSERT_EQ(memcmp(chunk.data, payload + off,
                                  chunk.size),
                           0);
        off += chunk.size;
        CFW_TEST_ASSERT_TRUE(pulls < 100);
    }
    CFW_TEST_ASSERT_EQ(off, sizeof(payload) - 1);
    CFW_TEST_ASSERT_TRUE(pulls > 1); /* 7-byte buffer forces many pulls */

    /* EOF is sticky. */
    CFW_TEST_ASSERT_EQ(cfw_file_stream_read(&stream, &chunk, &eof),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(eof);

    cfw_file_stream_close(&stream);
    cfw_file_stream_close(&stream); /* idempotent */
    cfw_file_stream_close(NULL);    /* tolerated */

    /* Empty file: first pull reports eof immediately. */
    {
        const char *empty = "cfw_test_fs_empty.bin";
        file = fopen(empty, "wb");
        CFW_TEST_ASSERT(file != NULL);
        CFW_TEST_ASSERT_EQ(fclose(file), 0);

        CFW_TEST_ASSERT_EQ(cfw_file_stream_init(&stream, empty, buf,
                                                sizeof(buf)),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_file_stream_read(&stream, &chunk, &eof),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(eof);
        cfw_file_stream_close(&stream);
        CFW_TEST_ASSERT_EQ(remove(empty), 0);
    }

    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}

static int test_fs_file_fstat_seek(void)
{
    const char *path = "cfw_test_fs_stat.bin";
    const char payload[] = "cfw-fstat-seek-payload";
    FILE *file;
    cfw_file_t fh;
    cfw_file_stat_t st;
    unsigned char buf[8];
    size_t got;

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    cfw_file_init(&fh);
    CFW_TEST_ASSERT_EQ(cfw_file_fstat(NULL, &st),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_fstat(&fh, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    {
        cfw_file_t bad;
        cfw_file_init(&bad);
        CFW_TEST_ASSERT_EQ(cfw_file_fstat(&bad, &st),
                           CFW_ERROR_INVALID_ARGUMENT);
        CFW_TEST_ASSERT_EQ(cfw_file_seek(&bad, 0),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    CFW_TEST_ASSERT_EQ(cfw_file_open(&fh, path), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_file_fstat(&fh, &st), CFW_OK);
    CFW_TEST_ASSERT_EQ(st.size, sizeof(payload) - 1);
    CFW_TEST_ASSERT_TRUE(st.mtime_seconds > 0);

    CFW_TEST_ASSERT_EQ(cfw_file_seek(NULL, 0),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_seek(&fh, (uint64_t)INT64_MAX + 1u),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Seek mid-file and read the tail. */
    CFW_TEST_ASSERT_EQ(cfw_file_seek(&fh, 5), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(got, sizeof(buf));
    CFW_TEST_ASSERT_EQ(memcmp(buf, payload + 5, got), 0);

    /* Seek back to the start and verify the head again. */
    CFW_TEST_ASSERT_EQ(cfw_file_seek(&fh, 0), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(got, sizeof(buf));
    CFW_TEST_ASSERT_EQ(memcmp(buf, payload, got), 0);

    /* Seeking past EOF yields EOF on the next read. */
    CFW_TEST_ASSERT_EQ(cfw_file_seek(&fh,
                                     (uint64_t)sizeof(payload)),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(got, 0);

    CFW_TEST_ASSERT_EQ(cfw_file_close(&fh), CFW_OK);
    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}

#if !defined(CFW_BUILD_SHARED)
static int test_fs_file_open_confined(void)
{
    const char *path = "cfw_test_fs_confined.bin";
    const char payload[] = "cfw-confined-payload";
    FILE *file;
    cfw_file_t fh;
    unsigned char buf[8];
    size_t got;
    size_t off = 0;

    file = fopen(path, "wb");
    CFW_TEST_ASSERT(file != NULL);
    CFW_TEST_ASSERT_EQ(fwrite(payload, 1, sizeof(payload) - 1, file),
                       sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(fclose(file), 0);

    cfw_file_init(&fh);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(NULL, ".", path),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, NULL, path),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", ""),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", "/abs"),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", "a/../b"),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", "a\\b"),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_FALSE(cfw_file_valid(&fh));

    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", path), CFW_OK);
    CFW_TEST_ASSERT_TRUE(cfw_file_valid(&fh));
    for (;;) {
        CFW_TEST_ASSERT_EQ(cfw_file_read(&fh, buf, sizeof(buf), &got),
                           CFW_OK);
        if (got == 0) {
            break;
        }
        CFW_TEST_ASSERT_TRUE(off + got <= sizeof(payload) - 1);
        CFW_TEST_ASSERT_EQ(memcmp(buf, payload + off, got), 0);
        off += got;
    }
    CFW_TEST_ASSERT_EQ(off, sizeof(payload) - 1);
    CFW_TEST_ASSERT_EQ(cfw_file_close(&fh), CFW_OK);

#if CFW_PLATFORM_POSIX
    /* A directory is not a regular file. */
    cfw_file_init(&fh);
    CFW_TEST_ASSERT_EQ(cfw_file_open_confined(&fh, ".", "."),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_FALSE(cfw_file_valid(&fh));
#endif

    CFW_TEST_ASSERT_EQ(remove(path), 0);
    return 0;
}
#endif /* !CFW_BUILD_SHARED */

const cfw_test_case cfw_test_fs_cases[] = {
    {"fs_file_size_success", test_fs_file_size_success},
    {"fs_file_size_errors", test_fs_file_size_errors},
    {"fs_is_directory", test_fs_is_directory},
    {"fs_file_open_read_close", test_fs_file_open_read_close},
    {"fs_file_open_errors", test_fs_file_open_errors},
    {"fs_file_stream", test_fs_file_stream},
    {"fs_file_fstat_seek", test_fs_file_fstat_seek},
#if !defined(CFW_BUILD_SHARED)
    {"fs_file_open_confined", test_fs_file_open_confined},
#endif
    {NULL, NULL},
};
