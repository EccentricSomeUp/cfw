"""Tests for the cfw_analyze static analyzer.

These tests feed small C fixture *strings* through the analyzer. They never
compile, link, or execute any C code; the analyzer is a pure source-text
static analyzer.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), os.pardir,
                                os.pardir, "tools", "analyzer"))

from cfw_analyze.driver import analyze_file, scan  # noqa: E402
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

    def test_numeric_macro_index_is_constant(self):
        src = """
#define CFW_TEST_INDEX 4
void f(void) {
    char buf[4];
    int i = CFW_TEST_INDEX;
    buf[i] = 0;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "OUT_OF_BOUNDS", "definite"))

    def test_enum_constant_index_is_constant(self):
        src = """
void f(void) {
    enum E { CFW_TEST_A = 4, CFW_TEST_B = CFW_TEST_A + 1 } x = CFW_TEST_B;
    char buf[4];
    buf[x] = 0;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "OUT_OF_BOUNDS", "definite"))


    def test_file_scope_enum_constant_index_is_constant(self):
        src = """
enum E { CFW_TEST_A = 4, CFW_TEST_B = CFW_TEST_A + 1 };
void f(void) {
    char buf[4];
    int i = CFW_TEST_B;
    buf[i] = 0;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "OUT_OF_BOUNDS", "definite"))

    def test_parameter_shadows_file_scope_enum_constant(self):
        src = """
enum E { CFW_TEST_A = 4 };
void f(int CFW_TEST_A) {
    char buf[4];
    int i = CFW_TEST_A;
    buf[i] = 0;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "OUT_OF_BOUNDS"))

    def test_cross_file_enum_constant_shared_by_scan(self):
        with tempfile.TemporaryDirectory() as td:
            with open(os.path.join(td, "limits.h"), "w") as f:
                f.write("enum Limits { CFW_TEST_LIMIT = 4 };\n")
            with open(os.path.join(td, "use.c"), "w") as f:
                f.write("""
void f(void) {
    char buf[4];
    int i = CFW_TEST_LIMIT;
    buf[i] = 0;
}
""")
            result = scan([td])
        self.assertIsNotNone(has(result.findings, "OUT_OF_BOUNDS",
                                  "definite"))


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

    def test_resolvable_preprocessor_in_function_not_partial(self):
        src = """
#define CFW_TEST_FEATURE 1
void f(void) {
#if CFW_TEST_FEATURE
    int x = 1;
#else
    int x = 2;
#endif
    (void)x;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("preprocessor" in p for p in funcs[0].partial))

    def test_unknown_preprocessor_branch_is_partial(self):
        src = """
void f(void) {
#if UNKNOWN_ANALYZER_FEATURE > 0
    int x = 1;
#else
    int x = 2;
#endif
    (void)x;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertTrue(any("preprocessor" in p for p in funcs[0].partial))

    def test_if_zero_else_branch_selected(self):
        src = """
#if 0
void bad(void) { }
#else
void good(void) { }
#endif
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual([f.name for f in funcs], ["good"])

    def test_ifndef_not_treated_as_if_expression(self):
        src = """
#ifndef _WIN32
void good(void) { }
#else
void bad(void) { }
#endif
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual([f.name for f in funcs], ["good"])
        self.assertFalse(any("preprocessor" in p for f in funcs
                             for p in f.partial))

    def test_transitive_numeric_macro_array_bound(self):
        src = """
#define CFW_SHA256_DIGEST_SIZE 32u
#define CFW_SIGNED_MAC_LEN CFW_SHA256_DIGEST_SIZE
void f(void) {
    unsigned char mac[CFW_SIGNED_MAC_LEN];
    mac[0] = 0;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("variably" in p for p in funcs[0].partial))

    def test_sizeof_string_array_in_bound(self):
        src = r"""
void f(void) {
    static const char guid[] = "0123456789abcdef0123456789abcdef";
    char material[256 + sizeof(guid) - 1];
    material[0] = 0;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("variably" in p for p in funcs[0].partial))

    def test_function_pointer_declarator_not_partial(self):
        src = """
void f(void) {
    void (*cb)(void) = 0;
    cb = 0;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("function-pointer" in p for p in funcs[0].partial))

    def test_string_macro_adjacent_literal(self):
        src = """
#define CFW_TEST_OUTSIDE_FILE "outside.txt"
int cfw_test_symlink(const char *a, const char *b);
void f(void) {
    (void)cfw_test_symlink("../" CFW_TEST_OUTSIDE_FILE, "link");
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_block_static_assert_and_alignas(self):
        src = """
void f(void) {
    _Static_assert(sizeof(int) == 4, "int size");
    _Alignas(16) char buf[16];
    buf[0] = 0;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(funcs[0].partial)

    def test_integer_constant_macros_and_casts(self):
        src = """
#define CFW_TEST_BLOCK_SIZE ((size_t)32)
#define CFW_TEST_NS INT64_C(1000000000)
void f(void) {
    char buf[CFW_TEST_BLOCK_SIZE];
    unsigned long long ns = CFW_TEST_NS;
    buf[0] = 0;
    (void)ns;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("variably" in p or "parse error" in p
                             for p in funcs[0].partial))

    def test_prefixed_string_literals_parse(self):
        src = r"""
void f(void) {
    const char *a = u8"utf8";
    const char *b = L"wide";
    wchar_t c = U'x';
    (void)a; (void)b; (void)c;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_generic_and_statement_expression_partial_not_parse_error(self):
        src = """
void f(int x) {
    int y = _Generic(x, int: 1, default: 0);
    int z = ({ int a = 1; a; });
    (void)y; (void)z;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertTrue(any("_Generic" in p for p in funcs[0].partial))
        self.assertTrue(any("statement expression" in p
                            for p in funcs[0].partial))
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_asm_and_computed_goto_partial_not_parse_error(self):
        src = """
void *f(void *p) {
    asm volatile ("nop");
    goto *p;
}
void *g(void) {
    void *p = &&done;
    goto *p;
done:
    return 0;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 2)
        self.assertTrue(any("inline assembly" in p for p in funcs[0].partial))
        self.assertTrue(any("computed goto" in p for p in funcs[0].partial))
        self.assertTrue(any("computed goto" in p for p in funcs[1].partial))
        self.assertFalse(any("parse error" in p
                             for f in funcs for p in f.partial))

    def test_va_arg_type_operand_not_parse_error(self):
        src = """
void f(va_list ap) {
    va_list aq;
    int n = va_arg(ap, int);
    char *s = va_arg(ap, char *);
    va_copy(aq, ap);
    va_end(aq);
    (void)n; (void)s;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_va_arg_pointer_may_be_null(self):
        src = """
void f(va_list ap) {
    char *p = va_arg(ap, char *);
    p[0] = 0;
}
"""
        fs = analyze(src)
        self.assertIsNotNone(has(fs, "NULL_DEREF", "possible"))

    def test_va_arg_pointer_check_ok(self):
        src = """
void f(va_list ap) {
    char *p = va_arg(ap, char *);
    if (p == NULL) {
        return;
    }
    p[0] = 0;
}
"""
        fs = analyze(src)
        self.assertIsNone(has(fs, "NULL_DEREF"))

    def test_block_typedef_cast_not_parse_error(self):
        src = """
void f(void) {
    typedef int myint;
    typedef int *myint_p;
    myint x = (myint)1;
    myint_p p = (myint_p)0;
    (void)x; (void)p;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_block_function_prototype_not_parse_error(self):
        src = """
void f(void) {
    int g(int);
    extern int h(const char *);
    int x, i = g(1);
    (void)h("x");
    (void)x; (void)i;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_declaration_attributes_not_parse_error(self):
        src = """
void f(void) {
    int x __attribute__((unused)) = 0;
    __declspec(deprecated) int y = 1;
    (void)x; (void)y;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_parenthesized_and_pointer_to_array_declarators(self):
        src = """
void f(void) {
    int (x) = 1;
    int ((y)) = 2;
    int (*p)[4];
    int (*(*pp))(void);
    (void)x; (void)y; (void)p; (void)pp;
}
"""
        funcs = parse_file("fixture.c", src)
        self.assertEqual(len(funcs), 1)
        self.assertFalse(any("parse error" in p for p in funcs[0].partial))

    def test_switch_break_is_not_loop_break(self):
        src = """
int f(int x) {
    switch (x) {
    case 0:
        break;
    default:
        break;
    }
    return 0;
}
"""
        _findings, nfuncs, npart, err = analyze_file("fixture.c", src, {})
        self.assertIsNone(err)
        self.assertEqual(nfuncs, 1)
        self.assertEqual(npart, 0)

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
