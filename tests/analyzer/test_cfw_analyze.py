"""Tests for the cfw_analyze static analyzer.

These tests feed small C fixture *strings* through the analyzer. They never
compile, link, or execute any C code; the analyzer is a pure source-text
static analyzer.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), os.pardir,
                                os.pardir, "tools", "analyzer"))

from cfw_analyze.driver import analyze_file  # noqa: E402
from cfw_analyze.parser import parse_file  # noqa: E402


def analyze(src, config=None):
    """Analyze a single in-memory source file; return finding list."""
    findings, _nfuncs, _npart, err = analyze_file("fixture.c", src,
                                               config or {})
    assert err is None, err
    return sorted(findings, key=lambda f: (f.line, f.rule, f.message))


def rules(findings):
    return sorted({f.rule for f in findings})


def has(findings, rule, confidence=None, line=None):
    for f in findings:
        if f.rule != rule:
            continue
        if confidence is not None and f.confidence != confidence:
            continue
        if line is not None and f.line != line:
            continue
        return f
    return None


class TestUseAfterFree(unittest.TestCase):
    def test_definite_uaf(self):
        src = """
void f(void) {
    int *p = malloc(10);
    free(p);
    p[0] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "USE_AFTER_FREE", "definite", 5))

    def test_safe_no_uaf(self):
        src = """
void f(void) {
    int *p = malloc(10);
    p[0] = 1;
    free(p);
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "USE_AFTER_FREE"))

    def test_branch_possible_uaf(self):
        src = """
void f(int c) {
    int *p = malloc(10);
    if (c) {
        free(p);
    }
    p[0] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "USE_AFTER_FREE", "possible"))

    def test_alias_uaf(self):
        src = """
void f(void) {
    int *p = malloc(10);
    int *q = p;
    free(q);
    p[0] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "USE_AFTER_FREE", "definite"))

    def test_early_return_no_uaf(self):
        src = """
void f(int c) {
    int *p = malloc(10);
    if (c) {
        free(p);
        return;
    }
    p[0] = 1;
    free(p);
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "USE_AFTER_FREE"))

    def test_null_assignment_does_not_repair_alias(self):
        src = """
void f(void) {
    int *p = malloc(10);
    int *q = p;
    free(p);
    p = NULL;
    q[0] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "USE_AFTER_FREE"))


class TestDoubleFree(unittest.TestCase):
    def test_double_free_possible(self):
        # malloc may return NULL; free(NULL) is safe, so 'possible'
        src = """
void f(void) {
    int *p = malloc(10);
    free(p);
    free(p);
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "DOUBLE_FREE", "possible"))

    def test_no_double_free(self):
        src = """
void f(void) {
    int *p = malloc(10);
    int *q = malloc(10);
    free(p);
    free(q);
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "DOUBLE_FREE"))

    def test_free_null_is_safe(self):
        src = """
void f(void) {
    int *p = NULL;
    free(p);
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "DOUBLE_FREE"))


class TestMemoryLeak(unittest.TestCase):
    def test_leak_definite(self):
        src = """
void f(void) {
    int *p = malloc(10);
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "MEMORY_LEAK", "definite"))

    def test_no_leak_when_freed(self):
        src = """
void f(void) {
    int *p = malloc(10);
    free(p);
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "MEMORY_LEAK"))

    def test_no_leak_when_returned(self):
        src = """
int *f(void) {
    int *p = malloc(10);
    return p;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "MEMORY_LEAK"))

    def test_branch_leak_possible(self):
        src = """
void f(int c) {
    int *p = malloc(10);
    if (c) {
        free(p);
    }
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "MEMORY_LEAK", "possible"))


class TestNullDeref(unittest.TestCase):
    def test_definite_null_deref(self):
        src = """
void f(void) {
    int *p = NULL;
    *p = 1;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "NULL_DEREF", "definite"))

    def test_checked_null_ok(self):
        src = """
void f(int *p) {
    if (p == NULL) {
        return;
    }
    *p = 1;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "NULL_DEREF"))

    def test_bang_check_ok(self):
        src = """
void f(int *p) {
    if (!p) {
        return;
    }
    p[0] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "NULL_DEREF"))

    def test_malloc_deref_without_check(self):
        src = """
void f(void) {
    int *p = malloc(10);
    p[0] = 1;
    free(p);
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "NULL_DEREF", "possible"))


class TestBounds(unittest.TestCase):
    def test_fixed_array_oob(self):
        src = """
void f(void) {
    char buf[10];
    buf[20] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "OUT_OF_BOUNDS", "definite"))

    def test_array_in_bounds(self):
        src = """
void f(void) {
    char buf[10];
    buf[9] = 1;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "OUT_OF_BOUNDS"))


class TestRealloc(unittest.TestCase):
    def test_realloc_failure_path_leak(self):
        src = """
void f(void) {
    int *p = malloc(10);
    int *q = realloc(p, 20);
    if (q == NULL) {
        return;
    }
    q[0] = 1;
    free(q);
}
"""
        fs = analyze(src)
        # on the failure path p is still live and the function returns
        # without freeing it -> leak; on success q is freed.
        self.assertIsNotNone(has(fs, "MEMORY_LEAK"))

    def test_realloc_use_after_free(self):
        src = """
void f(void) {
    int *p = malloc(10);
    int *q = realloc(p, 20);
    (void)q;
    p[0] = 1;
}
"""
        fs = analyze(src)
        # old object may be freed by realloc on the success path
        self.assertTrue(any(f.rule in ("USE_AFTER_FREE", "DOUBLE_FREE")
                            for f in fs))


class TestProjectWrappers(unittest.TestCase):
    def test_arena_alloc_out_param(self):
        src = """
int f(cfw_arena_t *a) {
    void *p;
    cfw_status_t st = cfw_arena_alloc(a, 64, &p);
    if (st != CFW_OK) {
        return st;
    }
    ((char *)p)[0] = 1;
    return CFW_OK;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "NULL_DEREF"))

    def test_arena_create_modeled(self):
        src = """
cfw_arena_t *f(void) {
    cfw_arena_t *a = cfw_arena_create(4096);
    if (a == NULL) {
        return NULL;
    }
    return a;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "MEMORY_LEAK"))


class TestTokenizer(unittest.TestCase):
    def test_comments_and_strings_ignored(self):
        src = r'''
void f(void) {
    // free(p); p[0] = 1; "use after free"
    /* malloc(1); double free(); */
    const char *s = "free(p); /* not a comment */";
    char c = '/';
    (void)s; (void)c;
    int *p = malloc(8);
    free(p);
}
'''
        fs = analyze(src)
        # only the real free exists; no UAF/double-free from comment text
        self.assertIsNone(has(fs, "USE_AFTER_FREE"))
        self.assertIsNone(has(fs, "DOUBLE_FREE"))

    def test_string_with_code_like_content(self):
        src = r'''
void f(void) {
    const char *s = "int *x = malloc(1); free(x); x[0]=0;";
    int *p = malloc(8);
    free(p);
    (void)s;
}
'''
        fs = analyze(src)
        self.assertIsNone(has(fs, "USE_AFTER_FREE"))


class TestParserRobustness(unittest.TestCase):
    def test_partial_function_marked(self):
        # VLA dimension is unsupported -> function marked partial, not
        # silently misanalyzed
        src = """
void f(size_t n) {
    char buf[n];
    buf[0] = 1;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertTrue(any("variably" in p for p in funcs[0].partial))

    def test_preprocessor_in_function_partial(self):
        src = """
void f(void) {
#if defined(FOO)
    int x = 1;
#endif
    (void)x;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertTrue(any("preprocessor" in p for p in funcs[0].partial))

    def test_constructs_parse_without_error(self):
        # casts, compound literals, comma exprs, adjacent strings,
        # function-pointer params, switch/goto
        src = r'''
typedef int (*cb_t)(void *);
void f(const void *data, size_t n, cb_t cb) {
    const unsigned char *p = (const unsigned char *)data;
    cfw_span_t s = (cfw_span_t){0};
    int eq = 0;
    if ((eq = 0, p != NULL) && n > 0) {
        const char *msg = "a"
                          "b";
        switch (n) {
        case 1:
            goto done;
        default:
            break;
        }
        cb(&s);
    }
done:
    ;
}
'''
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))


if __name__ == "__main__":
    unittest.main()
