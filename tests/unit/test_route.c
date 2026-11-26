/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static cfw_strview_t cfw_test_sv(const char *text)
{
    cfw_strview_t view;
    cfw_strview_from_cstr(&view, text);
    return view;
}

static cfw_status_t cfw_test_router_add(cfw_router_t *router,
                                        cfw_http_method_t method,
                                        const char *pattern, void *user,
                                        const cfw_route_t **out_route)
{
    cfw_strview_t view = cfw_test_sv(pattern);
    return cfw_router_add(router, method, &view, user, out_route);
}

static cfw_status_t cfw_test_match_param(const cfw_route_param_t *params,
                                         size_t written, const char *name,
                                         cfw_strview_t *value)
{
    cfw_strview_t n = cfw_test_sv(name);
    return cfw_route_match_param(params, written, &n, value);
}

static cfw_status_t cfw_test_target_path(const char *target,
                                         cfw_strview_t *path)
{
    cfw_strview_t t = cfw_test_sv(target);
    return cfw_http_target_path(&t, path);
}

static int test_route_basic_match(void)
{
    cfw_arena_t *arena = cfw_arena_create(4096);
    cfw_router_t router;
    cfw_route_match_t match;
    cfw_strview_t path;
    const cfw_route_t *route_a;
    const cfw_route_t *route_b;
    void *user_a = (void *)0x1234;
    void *user_b = (void *)0x5678;

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/users", user_a, &route_a),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/users/:id", user_b, &route_b),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_router_count(&router), 2);

    /* Exact literal match. */
    path = cfw_test_sv("/users");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == route_a);
    CFW_TEST_ASSERT_EQ(match.user == user_a, 1);
    CFW_TEST_ASSERT_EQ(match.param_count, 0);

    /* Param extraction. */
    path = cfw_test_sv("/users/42");
    {
        cfw_route_param_t params[4];
        cfw_strview_t value;
        CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                            &path, params, 4, &match),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(match.route == route_b);
        CFW_TEST_ASSERT_EQ(match.param_count, 1);
        CFW_TEST_ASSERT_EQ(match.params_written, 1);
        CFW_TEST_ASSERT_EQ(cfw_test_match_param(params, match.params_written,
                                                "id", &value),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(value.size, 2);
        CFW_TEST_ASSERT_EQ(memcmp(value.data, "42", 2), 0);
        /* Missing param name fails. */
        CFW_TEST_ASSERT_EQ(cfw_test_match_param(params, match.params_written,
                                                "name", &value),
                           CFW_ERROR_INVALID_ARGUMENT);
    }

    /* Method mismatch → no match. */
    path = cfw_test_sv("/users");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_DELETE,
                                        &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == NULL);

    /* Trailing slash is a different resource. */
    path = cfw_test_sv("/users/");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == NULL);

    /* Extra segment → no match. */
    path = cfw_test_sv("/users/1/posts");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == NULL);

    /* Case-sensitive literals. */
    path = cfw_test_sv("/Users");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == NULL);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_route_root_and_wildcard(void)
{
    cfw_arena_t *arena = cfw_arena_create(4096);
    cfw_router_t router;
    cfw_route_match_t match;
    cfw_strview_t path;

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/", NULL, NULL),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_UNKNOWN, "/any", NULL, NULL),
                       CFW_OK);

    path = cfw_test_sv("/");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route != NULL);
    CFW_TEST_ASSERT_EQ(match.param_count, 0);

    /* "/x" matches nothing: the wildcard route is method-any, not
     * path-any. */
    path = cfw_test_sv("/x");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == NULL);

    /* UNKNOWN method route matches any method. */
    path = cfw_test_sv("/any");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_DELETE,
                                        &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route != NULL);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_route_priority_and_params(void)
{
    cfw_arena_t *arena = cfw_arena_create(8192);
    cfw_router_t router;
    cfw_route_match_t match;
    cfw_strview_t path;
    const cfw_route_t *first;
    const cfw_route_t *second;
    int marker_first = 1;
    int marker_second = 2;

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    /* Literal registered before param route wins for literal path. */
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/items/special", &marker_first, &first),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/items/:id", &marker_second, &second),
                       CFW_OK);

    path = cfw_test_sv("/items/special");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == first);
    CFW_TEST_ASSERT_EQ(match.user == &marker_first, 1);

    path = cfw_test_sv("/items/other");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path, NULL, 0, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route == second);
    CFW_TEST_ASSERT_EQ(match.user == &marker_second, 1);

    /* Multi-param pattern. */
    {
        cfw_route_param_t params[2];
        cfw_strview_t v1;
        cfw_strview_t v2;
        const cfw_route_t *r3;
        CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/a/:x/b/:y", NULL, &r3),
                           CFW_OK);
        path = cfw_test_sv("/a/1/b/2");
        CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                            &path, params, 2, &match),
                           CFW_OK);
        CFW_TEST_ASSERT_TRUE(match.route == r3);
        CFW_TEST_ASSERT_EQ(cfw_test_match_param(params, match.params_written,
                                                "x", &v1),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_test_match_param(params, match.params_written,
                                                "y", &v2),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(v1.size, 1);
        CFW_TEST_ASSERT_EQ(v2.size, 1);
        CFW_TEST_ASSERT_EQ(v1.data[0], '1');
        CFW_TEST_ASSERT_EQ(v2.data[0], '2');
    }

    cfw_arena_destroy(arena);
    return 0;
}

static int test_route_capacity_truncation(void)
{
    cfw_arena_t *arena = cfw_arena_create(4096);
    cfw_router_t router;
    cfw_route_match_t match;
    cfw_strview_t path;
    cfw_route_param_t params[1];
    cfw_strview_t value;

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET, "/a/:x/b/:y", NULL, NULL),
                       CFW_OK);

    path = cfw_test_sv("/a/1/b/2");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET, &path,
                                        params, 1, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route != NULL);
    CFW_TEST_ASSERT_EQ(match.param_count, 2);
    CFW_TEST_ASSERT_EQ(match.params_written, 1);
    /* First param still retrievable. */
    CFW_TEST_ASSERT_EQ(cfw_test_match_param(params, match.params_written,
                                            "x", &value),
                       CFW_OK);

    cfw_arena_destroy(arena);
    return 0;
}

static int test_route_registration_validation(void)
{
    cfw_arena_t *arena = cfw_arena_create(4096);
    cfw_router_t router;
    static const char *bad[] = {
        "",           /* empty */
        "users",      /* no leading slash */
        "/a//b",      /* empty segment */
        "/a/",        /* trailing slash → empty segment */
        "/:1bad",     /* param name starts with digit */
        "/:",         /* empty param name */
        "/a b",       /* space in literal */
        "/a?b",       /* '?' in literal */
        "/a\x01" "b", /* control char */
        "/:x/:x",     /* duplicate param name */
    };
    size_t i;

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cfw_strview_t pattern = cfw_test_sv(bad[i]);
        CFW_TEST_ASSERT_EQ(cfw_router_add(&router, CFW_HTTP_METHOD_GET,
                                        &pattern, NULL, NULL),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    CFW_TEST_ASSERT_EQ(cfw_router_count(&router), 0);

    /* Valid edge patterns. */
    {
        static const char *good[] = {
            "/",
            "/a-b_c.d~e",
            "/:id",
            "/files/:name",
            "/a%20b",
        };
        for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            cfw_strview_t pattern = cfw_test_sv(good[i]);
            CFW_TEST_ASSERT_EQ(cfw_router_add(&router, CFW_HTTP_METHOD_GET,
                                            &pattern, NULL, NULL),
                               CFW_OK);
        }
        CFW_TEST_ASSERT_EQ(cfw_router_count(&router),
                           sizeof(good) / sizeof(good[0]));
    }

    cfw_arena_destroy(arena);
    return 0;
}

static int test_route_match_param_edge(void)
{
    cfw_route_match_t match;
    cfw_strview_t value;

    memset(&match, 0, sizeof(match));
    CFW_TEST_ASSERT_EQ(cfw_test_match_param(NULL, 0, "x", &value),
                       CFW_ERROR_INVALID_ARGUMENT);
    {
        cfw_strview_t root = cfw_test_sv("/");
        CFW_TEST_ASSERT_EQ(cfw_router_match(NULL, CFW_HTTP_METHOD_GET,
                                            &root, NULL, 0, &match),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(NULL, CFW_HTTP_METHOD_GET,
                                    "/", NULL, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_http_target_path(void)
{
    cfw_strview_t path;

    /* origin-form with query */
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("/a/b?x=1&y=2", &path),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(path.size, 4);
    CFW_TEST_ASSERT_EQ(memcmp(path.data, "/a/b", 4), 0);

    /* origin-form without query */
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("/", &path),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(path.size, 1);

    /* absolute-form */
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("http://example.com:8080/p?q=1", &path),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(path.size, 2);
    CFW_TEST_ASSERT_EQ(memcmp(path.data, "/p", 2), 0);

    /* absolute-form https */
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("https://h/a/b", &path),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(path.size, 4);

    /* authority-form and asterisk-form rejected */
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("example.com:443", &path),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("*", &path),
                       CFW_ERROR_INVALID_ARGUMENT);
    /* absolute-form without path */
    CFW_TEST_ASSERT_EQ(cfw_test_target_path("http://h", &path),
                       CFW_ERROR_INVALID_ARGUMENT);

    return 0;
}

static int test_route_param_decode(void)
{
    cfw_arena_t *arena = cfw_arena_create(4096);
    cfw_router_t router;
    cfw_route_match_t match;
    cfw_route_param_t params[4];
    cfw_strview_t path;
    cfw_strview_t name;
    char dst[64];
    size_t out_len = 0;
    cfw_strview_t raw;

    CFW_TEST_ASSERT_TRUE(arena != NULL);
    CFW_TEST_ASSERT_EQ(cfw_router_init(&router, arena), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_test_router_add(&router, CFW_HTTP_METHOD_GET,
                                           "/files/:name", NULL, NULL),
                       CFW_OK);

    path = cfw_test_sv("/files/report%20final%21.txt");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                        &path, params, 4, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(match.route != NULL);

    /* Raw value stays percent-encoded. */
    name = cfw_test_sv("name");
    CFW_TEST_ASSERT_EQ(cfw_route_match_param(params, match.params_written,
                                             &name, &raw),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(raw.size == strlen("report%20final%21.txt"));

    /* Decoded value via the convenience helper. */
    CFW_TEST_ASSERT_EQ(cfw_route_param_decode(
                           params, match.params_written, &name, dst,
                           sizeof(dst), &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(out_len, strlen("report final!.txt"));
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "report final!.txt",
                                out_len + 1) == 0);

    /* '+' stays literal for path params (no form semantics). */
    path = cfw_test_sv("/files/a+b");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                        &path, params, 4, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_route_param_decode(
                           params, match.params_written, &name, dst,
                           sizeof(dst), &out_len, 0),
                       CFW_OK);
    CFW_TEST_ASSERT_TRUE(memcmp(dst, "a+b", 4) == 0);

    /* Flags are honored: backslash rejected. */
    path = cfw_test_sv("/files/a%5Cb");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                        &path, params, 4, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_route_param_decode(
                           params, match.params_written, &name, dst,
                           sizeof(dst), &out_len,
                           CFW_URI_DECODE_REJECT_BACKSLASH),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Malformed escape propagates as an error. */
    path = cfw_test_sv("/files/a%2");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                        &path, params, 4, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_route_param_decode(
                           params, match.params_written, &name, dst,
                           sizeof(dst), &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Unknown param name → error. */
    name = cfw_test_sv("missing");
    CFW_TEST_ASSERT_EQ(cfw_route_param_decode(
                           params, match.params_written, &name, dst,
                           sizeof(dst), &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Undersized dst → error. */
    name = cfw_test_sv("name");
    path = cfw_test_sv("/files/abc");
    CFW_TEST_ASSERT_EQ(cfw_router_match(&router, CFW_HTTP_METHOD_GET,
                                        &path, params, 4, &match),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_route_param_decode(
                           params, match.params_written, &name, dst, 3,
                           &out_len, 0),
                       CFW_ERROR_INVALID_ARGUMENT);

    cfw_arena_destroy(arena);
    return 0;
}

const cfw_test_case cfw_test_route_cases[] = {
    {"route_basic_match", test_route_basic_match},
    {"route_root_and_wildcard", test_route_root_and_wildcard},
    {"route_priority_and_params", test_route_priority_and_params},
    {"route_capacity_truncation", test_route_capacity_truncation},
    {"route_registration_validation", test_route_registration_validation},
    {"route_match_param_edge", test_route_match_param_edge},
    {"route_param_decode", test_route_param_decode},
    {"http_target_path", test_http_target_path},
    {NULL, NULL},
};
