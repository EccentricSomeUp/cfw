"""Bounded recursive-descent parser for a useful subset of C11.

The parser is deliberately not a full C parser. It handles the constructs
needed for intra-procedural memory analysis and marks anything it cannot
handle as unsupported/partial instead of guessing.

Supported:
  * function definitions with prototype-style parameter lists
  * local declarations with pointer depth, array dims, and initializers
  * compound statements, if/else, while, do/while, for, switch
  * return/break/continue/goto and case/default labels
  * expressions: assignment, ternary, logical/arithmetic/bitwise ops,
    calls, indexing, member access, address-of/deref, casts (heuristic),
    sizeof, _Alignof, prefix/postfix ++/--, enum constants, compound
    literals, and C11 integer constant macros (INT8_C, UINT64_C, ...)
  * block-scope _Static_assert and _Alignas declarations
  * block-scope typedefs and function prototype declarations
  * encoding-prefixed string/character literals (u8"...", L'x', ...)
  * va_arg type-name operands and va_start/va_end/va_copy calls

Not supported (function marked partial or unsupported):
  * unresolved preprocessor conditionals (known #if/#ifdef branches are
    filtered before parsing)
  * statement expressions, _Generic, computed goto, inline asm,
    VLA dimensions that are not integer constants
  * K&R-style definitions, nested functions, C++ constructs
"""

from __future__ import annotations

import errno
import platform
import re
import struct
import sys
from dataclasses import dataclass, field

from .lexer import Token, tokenize

# Expression tuple formats:
#   ('num', text, line) ('str', text, line) ('var', name, line)
#   ('call', func, [args], line) ('assign', op, lhs, rhs, line)
#   ('binary', op, l, r, line) ('unary', op, e, line)
#   ('postfix', op, e, line) ('member', obj, name, arrow, line)
#   ('index', obj, idx, line) ('cast', type_str, operand, line)
#   ('cond', c, t, f, line) ('sizeof', expr_or_None, line)
#   ('addr', expr, line) ('deref' is ('unary','*',e)) ('unknown', line)
#   ('va_arg', ap_expr, type_text, line)
#
# Statement tuple formats:
#   ('block', [stmts], line) ('if', cond, then, els, line)
#   ('while', cond, body, line) ('do', body, cond, line)
#   ('for', init_decls_or_None, init_exprs, cond, incr_exprs, body, line)
#   ('switch', expr, body, line) ('return', expr_or_None, line)
#   ('break', line) ('continue', line) ('goto', label, line)
#   ('label', name, line) ('case', expr, line) ('default', line)
#   ('expr', expr, line) ('decl', [VarDecl...], line) ('empty', line)
#   ('opaque', line)   -- unparseable statement; analysis escapes state

SPEC_KWS = frozenset(
    "auto register static extern typedef inline const volatile restrict "
    "_Noreturn _Thread_local _Atomic _Alignas _Alignof signed unsigned "
    "void char short int long float double _Bool _Complex _Imaginary "
    "struct union enum".split()
)

DECL_START_KWS = SPEC_KWS | {"if", "while"}  # 'if'/'while' never start decls;
# kept out below -- see is_decl_start.

# Keywords that can legitimately begin a declaration in statement position.
# _Alignof is excluded because it is an expression operator; _Alignas is a
# declaration specifier and may open a declaration (_Alignas(16) int x;).
DECL_SPEC_KWS = SPEC_KWS - {"_Alignof"}

QUALIFIER_KWS = frozenset({"const", "volatile", "restrict", "_Atomic"})

# Common libc/project typedef names usable for cast detection when the
# typedef itself is not visible in the scanned files.
BUILTIN_TYPE_NAMES = frozenset(
    """
    size_t ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t ssize_t
    int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t
    int_least8_t int_least16_t int_least32_t int_least64_t
    uint_least8_t uint_least16_t uint_least32_t uint_least64_t
    int_fast8_t int_fast16_t int_fast32_t int_fast64_t
    uint_fast8_t uint_fast16_t uint_fast32_t uint_fast64_t
    FILE cfw_status_t cfw_span_t va_list wint_t wctrans_t wctype_t
    """.split()
)

# Sizes for common fixed-width typedef names (LP64 layout).
BUILTIN_TYPE_SIZES = {
    "int8_t": 1, "uint8_t": 1, "int_least8_t": 1, "uint_least8_t": 1,
    "int_fast8_t": 1, "uint_fast8_t": 1,
    "int16_t": 2, "uint16_t": 2, "int_least16_t": 2, "uint_least16_t": 2,
    "int_fast16_t": 2, "uint_fast16_t": 2,
    "int32_t": 4, "uint32_t": 4, "int_least32_t": 4, "uint_least32_t": 4,
    "int_fast32_t": 4, "uint_fast32_t": 4,
    "int64_t": 8, "uint64_t": 8, "int_least64_t": 8, "uint_least64_t": 8,
    "int_fast64_t": 8, "uint_fast64_t": 8,
    "intmax_t": 8, "uintmax_t": 8, "size_t": 8, "ptrdiff_t": 8,
    "intptr_t": 8, "uintptr_t": 8, "ssize_t": 8,
    "wchar_t": 4, "wint_t": 4, "wctrans_t": 8, "wctype_t": 8,
    "bool": 1, "cfw_status_t": 4,
}

_DECL_QUALIFIER_WORDS = frozenset(
    "const volatile restrict static extern register auto inline typedef "
    "_Atomic _Alignas _Noreturn".split())


def decl_type_size(spec_texts):
    """Rough element size in bytes for builtin declaration specifiers.

    Returns an int for the common LP64 layout or None when the size is
    unknown (typedef names, struct/union/enum, _Complex, void)."""
    s = " " + " ".join(spec_texts) + " "
    if " struct " in s or " union " in s or " enum " in s:
        return None
    if " _Complex " in s or " _Imaginary " in s:
        return None
    if " long double " in s:
        return 16
    if " long " in s:
        return 8
    if " short " in s:
        return 2
    if " char " in s or " _Bool " in s:
        return 1
    if " double " in s:
        return 8
    if " int " in s or " float " in s or " signed " in s \
            or " unsigned " in s:
        return 4
    core = [t for t in spec_texts if t not in _DECL_QUALIFIER_WORDS]
    if len(core) == 1:
        return BUILTIN_TYPE_SIZES.get(core[0])
    return None

# C11 <stdint.h>/<inttypes.h> integer constant macros. Their values are
# the literal argument widened to the named type; for source-level analysis
# the integer value is what matters.
INTEGER_CONSTANT_MACROS = frozenset(
    """
    INT8_C INT16_C INT32_C INT64_C INTMAX_C
    UINT8_C UINT16_C UINT32_C UINT64_C UINTMAX_C
    INT_LEAST8_C INT_LEAST16_C INT_LEAST32_C INT_LEAST64_C
    UINT_LEAST8_C UINT_LEAST16_C UINT_LEAST32_C UINT_LEAST64_C
    INT_FAST8_C INT_FAST16_C INT_FAST32_C INT_FAST64_C
    UINT_FAST8_C UINT_FAST16_C UINT_FAST32_C UINT_FAST64_C
    PTRDIFF_C SIZE_C WCHAR_C WINT_C SIG_ATOMIC_C BOOL_C
    """.split()
)

VA_ARG_NAMES = frozenset({"va_arg", "__builtin_va_arg"})


def _standard_integer_macros():
    """C99/C11 integer-limit macros needed by #if expressions and constant
    folding. Widths follow the host running the scan, matching the existing
    platform-aware preprocessor model."""
    bits = 64 if sys.maxsize > 2**32 - 1 else 32
    long_bits = struct.calcsize("l") * 8
    signed = {
        "SCHAR": (8, -0x80, 0x7F),
        "SHRT": (16, -(1 << 15), (1 << 15) - 1),
        "INT": (32, -(1 << 31), (1 << 31) - 1),
        "LONG": (long_bits, -(1 << (long_bits - 1)),
                 (1 << (long_bits - 1)) - 1),
        "LLONG": (64, -(1 << 63), (1 << 63) - 1),
        "INTMAX": (64, -(1 << 63), (1 << 63) - 1),
    }
    values = {
        "CHAR_BIT": 8,
        "UCHAR_MAX": 0xFF,
        "USHRT_MAX": 0xFFFF,
        "UINT_MAX": 0xFFFFFFFF,
        "ULONG_MAX": (1 << long_bits) - 1,
        "ULLONG_MAX": (1 << 64) - 1,
        "UINTMAX_MAX": (1 << 64) - 1,
        "BOOL_MAX": 1,
    }
    for prefix, (width, lo, hi) in signed.items():
        values[f"{prefix}_MIN"] = lo
        values[f"{prefix}_MAX"] = hi
    values.update({
        "INT8_MIN": -(1 << 7), "INT8_MAX": (1 << 7) - 1,
        "INT16_MIN": -(1 << 15), "INT16_MAX": (1 << 15) - 1,
        "INT32_MIN": -(1 << 31), "INT32_MAX": (1 << 31) - 1,
        "INT64_MIN": -(1 << 63), "INT64_MAX": (1 << 63) - 1,
        "UINT8_MAX": 0xFF, "UINT16_MAX": 0xFFFF,
        "UINT32_MAX": 0xFFFFFFFF, "UINT64_MAX": (1 << 64) - 1,
        "UINTPTR_MAX": (1 << bits) - 1,
        "INTPTR_MAX": (1 << (bits - 1)) - 1,
        "INTPTR_MIN": -(1 << (bits - 1)),
        "PTRDIFF_MAX": (1 << (bits - 1)) - 1,
        "PTRDIFF_MIN": -(1 << (bits - 1)),
        "SIZE_MAX": (1 << bits) - 1,
    })
    return values


def host_preprocessor_macros():
    """Best-effort compiler/platform macros for the host running the scan.

    The analyzer is platform-aware only for resolving preprocessor
    conditionals; it still analyzes the selected branch's C text.
    """
    macros = {
        "__STDC__": 1,
        "__GNUC__": 12,
        "EAGAIN": errno.EAGAIN,
        "EWOULDBLOCK": getattr(errno, "EWOULDBLOCK", errno.EAGAIN),
    }
    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        macros.update({"__x86_64__": 1, "__amd64__": 1})
    elif machine in ("aarch64", "arm64"):
        macros["__aarch64__"] = 1
    elif machine.startswith("arm"):
        macros["__arm__"] = 1
    elif machine.startswith("riscv"):
        macros["__riscv"] = 1
        macros["__riscv_xlen"] = 64 if "64" in machine else 32
    elif machine in ("ppc64", "ppc64le", "powerpc64"):
        macros.update({"__powerpc64__": 1, "__PPC64__": 1})
    elif machine == "s390x":
        macros["__s390x__"] = 1
    elif machine in ("i386", "i486", "i586", "i686", "x86"):
        macros["__i386__"] = 1
    if sys.byteorder == "little":
        macros.update({
            "__ORDER_LITTLE_ENDIAN__": 1234,
            "__ORDER_BIG_ENDIAN__": 4321,
            "__ORDER_PDP_ENDIAN__": 3412,
            "__BYTE_ORDER__": 1234,
        })
    elif sys.byteorder == "big":
        macros.update({
            "__ORDER_LITTLE_ENDIAN__": 1234,
            "__ORDER_BIG_ENDIAN__": 4321,
            "__ORDER_PDP_ENDIAN__": 3412,
            "__BYTE_ORDER__": 4321,
        })
    if sys.platform.startswith("linux"):
        macros["__linux__"] = 1
    elif sys.platform == "darwin":
        macros["__APPLE__"] = 1
    elif sys.platform.startswith("win"):
        macros["_WIN32"] = 1
        if sys.maxsize > 2**32 - 1:
            macros["_WIN64"] = 1
    if sys.platform.startswith(("linux", "freebsd", "openbsd", "netbsd",
                                "dragonfly")):
        macros["__unix__"] = 1
    return macros


def preprocessor_defines(numeric_macros=None, external_defines=None):
    """Environment for #if/#ifdef evaluation.

    Values are ints for numeric macros and None for defined-but-not-numeric
    macros (for example string-valued compile definitions). Source include
    guards are intentionally not seeded here: ``#ifndef GUARD`` must remain
    true when the guard is only defined later in the same header.
    """
    env = _standard_integer_macros()
    env.update(host_preprocessor_macros())
    for name, value in (numeric_macros or {}).items():
        env[name] = value
    for spec in external_defines or ():
        name, sep, value = spec.partition("=")
        if not name:
            continue
        if not sep:
            env[name] = None
            continue
        value = value.strip().strip('"').strip("'")
        try:
            env[name] = int(value.rstrip("uUlLfF"), 0)
            continue
        except ValueError:
            pass
        try:
            value_tokens = [t for t in tokenize(value) if t.kind != "eof"]
            evaluated = _Parser(value_tokens, set())._pp_eval_expr(
                value_tokens, env, unknown_ok=True)
            env[name] = evaluated if isinstance(evaluated, int) else None
        except Exception:
            env[name] = None
    return env


def filter_tokens(tokens, typedef_names, numeric_macros=None,
                  extra_defines=None):
    """Return (active_tokens, unknown_pp_lines) for preprocessor filtering.

    This is the same conditional resolution used by ``parse_file``; it is
    exposed so the scan driver can collect file-scope constants from the
    active branches of every file before analyzing functions.
    """
    typedef_names = set(typedef_names) | BUILTIN_TYPE_NAMES
    pp_env = preprocessor_defines(numeric_macros=numeric_macros,
                                  external_defines=extra_defines)
    parser = _Parser(tokens, typedef_names, numeric_macros)
    return parser._pp_filter(tokens, pp_env)


def _strip_line_comment(text):
    out = []
    i = 0
    n = len(text)
    quote = None
    esc = False
    while i < n:
        c = text[i]
        if quote is not None:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == quote:
                quote = None
            i += 1
            continue
        if c in "\"'":
            quote = c
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            break
        out.append(c)
        i += 1
    return "".join(out).strip()


def _macro_definition_parts(text):
    match = re.match(r"#\s*define\s+([A-Za-z_]\w*)", text)
    if not match:
        return None
    rest = text[match.end():]
    if rest.startswith("("):
        return match.group(1), None  # function-like macro
    return match.group(1), _strip_line_comment(rest)


def collect_macro_definitions(tokens):
    """Collect object-like #define bodies (raw text, unresolved)."""
    defs = {}
    for t in tokens:
        if t.kind != "pp":
            continue
        text = t.text.strip()
        if not text.startswith("#define"):
            continue
        parts = _macro_definition_parts(text)
        if parts is None:
            continue
        name, value = parts
        if value is None:
            continue
        defs[name] = value
    return defs


def collect_numeric_macros(tokens, seed=None):
    """Collect numeric object-like #define constants from one token stream."""
    return resolve_numeric_macros(collect_macro_definitions(tokens), seed)


def resolve_numeric_macros(definitions, seed=None):
    """Resolve raw object-like macro bodies to integers, iterating to a
    fixed point so forward and cross-file references work."""
    env = _standard_integer_macros()
    env.update(host_preprocessor_macros())
    for name, value in (seed or {}).items():
        if isinstance(value, int):
            env[name] = value
    resolved = {}
    pending = dict(definitions)
    for _ in range(len(pending) + 1):
        changed = False
        for name, value in list(pending.items()):
            if not value:
                continue
            try:
                expr_tokens = tokenize(value)
                value_tokens = [t for t in expr_tokens if t.kind != "eof"]
                v = _Parser(value_tokens, set())._pp_eval_expr(
                    value_tokens, env, unknown_ok=True)
            except Exception:
                v = None
            if isinstance(v, int):
                resolved[name] = v
                env[name] = v
                del pending[name]
                changed = True
        if not changed:
            break
    return resolved


BIN_PREC = {
    "||": 3, "&&": 4, "|": 5, "^": 6, "&": 7,
    "==": 8, "!=": 8,
    "<": 9, "<=": 9, ">": 9, ">=": 9,
    "<<": 10, ">>": 10,
    "+": 11, "-": 11,
    "*": 12, "/": 12, "%": 12,
}
RIGHT_ASSOC = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
               "<<=", ">>=", "?"}
ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
              "<<=", ">>="}


@dataclass
class VarDecl:
    name: str          # resolved unique name
    ptr_depth: int
    dims: list         # list of size expr tuples or None entries
    init: object       # expr tuple, 'complex' marker, or None
    is_funcptr: bool
    line: int
    vla: bool = False
    is_function: bool = False
    type_size: object = None  # element size in bytes or None


@dataclass
class FuncDef:
    name: str
    params: list       # resolved unique names
    body: tuple        # statement tuple
    line: int
    partial: list = field(default_factory=list)
    file: str = ""


class ParseError(Exception):
    pass


class _Parser:
    def __init__(self, tokens, typedef_names, numeric_macros=None,
                 string_macros=None):
        self.toks = tokens
        self.i = 0
        self.partial = []
        self.typedef_names = typedef_names
        self.numeric_macros = dict(numeric_macros or {})
        self.string_macros = dict(string_macros or {})
        self.constants = dict(self.numeric_macros)
        self.object_sizes = {}
        self.scope = [{}]          # name -> unique name
        self.const_scopes = [{}]
        self.typedef_scopes = [set()]
        self.visible_typedef_names = set(typedef_names)
        self._decl_typedef = False
        self.uid = 0
        self.func_name = ""
        self.label_seen = set()

    # ---- token helpers -------------------------------------------------
    def peek(self, k=0):
        j = self.i + k
        if j >= len(self.toks):
            return self.toks[-1]
        return self.toks[j]

    def next(self):
        t = self.toks[self.i]
        if t.kind != "eof":
            self.i += 1
        return t

    def at_punct(self, s, k=0):
        t = self.peek(k)
        return t.kind == "punct" and t.text == s

    def at_kw(self, s, k=0):
        t = self.peek(k)
        return t.kind == "kw" and t.text == s

    def at_id(self, k=0):
        return self.peek(k).kind == "id"

    def skip_pp(self):
        while self.peek().kind == "pp":
            self.partial.append("preprocessor line inside function")
            self.next()

    def expect_punct(self, s):
        t = self.next()
        if not (t.kind == "punct" and t.text == s):
            raise ParseError(f"expected '{s}' got '{t.text}' at {t.line}")
        return t

    def skip_balanced(self, open_s, close_s):
        """Consume tokens from current '(' or '{' through its match."""
        depth = 0
        while True:
            t = self.next()
            if t.kind == "eof":
                raise ParseError("unbalanced")
            if t.kind == "punct":
                if t.text == open_s:
                    depth += 1
                elif t.text == close_s:
                    depth -= 1
                    if depth == 0:
                        return

    # ---- declarations --------------------------------------------------
    def is_type_like(self, t):
        if t.kind == "kw" and t.text in SPEC_KWS:
            return True
        if t.kind == "id" and (t.text in self.visible_typedef_names
                               or t.text in BUILTIN_TYPE_NAMES
                               or t.text.endswith("_t")
                               or t.text.startswith("cfw_")
                               or t.text.isupper()):
            return True
        return False

    def is_decl_start(self):
        t = self.peek()
        if t.kind == "pp":
            return False
        if t.kind == "kw":
            # only declaration-specifier keywords can open a declaration;
            # expression keywords (sizeof, _Alignof, _Generic, ...) cannot
            return t.text in DECL_SPEC_KWS
        if t.kind == "id":
            if t.text in ("__attribute__", "__declspec"):
                return True
            n1 = self.peek(1)
            if n1.kind == "id":
                return True
            if n1.kind == "punct" and n1.text == "*":
                return True
            if n1.kind == "punct" and n1.text == "(":
                return self._funcptr_decl_ahead()
        return False

    def _funcptr_decl_ahead(self):
        """True if tokens look like 'T (*name)(params)' -- a function
        pointer declarator. The '(' must be followed by '*'* name ')' and
        then another '('. This distinguishes 'f(*p);' calls from
        'T (*fp)(void);' declarations."""
        j = self.i + 1
        n = len(self.toks)
        if j >= n or self.toks[j].kind != "punct" \
                or self.toks[j].text != "(":
            return False
        j += 1
        while j < n:
            t = self.toks[j]
            if t.kind == "punct" and t.text == "*":
                j += 1
            elif t.kind == "kw" and t.text in QUALIFIER_KWS:
                j += 1
            elif t.kind == "id":
                break
            else:
                return False
        else:
            return False
        # find ')' matching the '(' at self.i + 1
        depth = 0
        k = self.i + 1
        while k < n:
            t = self.toks[k]
            if t.kind == "punct":
                if t.text == "(":
                    depth += 1
                elif t.text == ")":
                    depth -= 1
                    if depth == 0:
                        break
            k += 1
        if k >= n:
            return False
        m = k + 1
        while m < n and self.toks[m].kind == "pp":
            m += 1
        return m < n and self.toks[m].kind == "punct" \
            and self.toks[m].text == "("

    def skip_specifiers(self):
        """Consume declaration specifiers; stop at first declarator token.

        Type-like identifiers (typedef names, ``*_t``, ``cfw_*``) are only
        consumed as specifiers until the first real type has been seen;
        afterwards they are declarator names (e.g. ``int cfw_x;``).
        Returns True if the specifiers formed a complete declaration
        (e.g. ``enum { ... };``) with no declarator following.
        """
        seen_type = False
        while True:
            t = self.peek()
            if t.kind == "pp":
                self.skip_pp()
                continue
            if t.kind == "kw" and t.text in SPEC_KWS:
                if t.text == "typedef":
                    self._decl_typedef = True
                self.next()
                if t.text in ("struct", "union", "enum"):
                    if self.peek().kind == "id":
                        self.next()
                    if self.at_punct("{"):
                        if t.text == "enum":
                            self._skip_enum_body()
                        else:
                            self.skip_balanced("{", "}")
                    seen_type = True
                elif t.text in ("void", "char", "short", "int", "long",
                                "float", "double", "signed", "unsigned",
                                "_Bool", "_Complex", "_Imaginary"):
                    seen_type = True
                elif t.text == "_Atomic" and self.at_punct("("):
                    self.skip_balanced("(", ")")
                    seen_type = True
                elif t.text == "_Alignas" and self.at_punct("("):
                    self.skip_balanced("(", ")")
                continue
            if (not seen_type and t.kind == "id"
                    and (self.is_type_like(t)
                         or self._id_is_probable_type())):
                self.next()
                seen_type = True
                continue
            if t.kind == "id" and t.text in ("__attribute__", "__declspec"):
                self.next()
                if self.at_punct("("):
                    self.skip_balanced("(", ")")
                continue
            break
        # enum/struct/union definitions with no declarator are complete
        # declarations; signal that no declarator should be parsed.
        return seen_type and self.at_punct(";")

    def _id_is_probable_type(self):
        """Current token is an id; True if the following tokens look like
        a declarator (name, '*', or funcptr), meaning the id is probably
        an unknown typedef name rather than a variable."""
        n1 = self.peek(1)
        if n1.kind == "id":
            return True
        if n1.kind == "punct" and n1.text == "*":
            return True
        if n1.kind == "punct" and n1.text == "(":
            return self._funcptr_decl_ahead()
        return False

    def _skip_enum_body(self):
        """Consume an enum { ... } body and record integer constants."""
        if not self.at_punct("{"):
            return
        self.next()  # consume '{'
        next_val = 0
        while not self.at_punct("}"):
            if self.peek().kind == "eof":
                raise ParseError("unterminated enum")
            if self.peek().kind == "punct" and self.peek().text == ",":
                self.next()
                continue
            if self.peek().kind != "id":
                self.next()
                continue
            name = self.next().text
            val = next_val
            if self.at_punct("="):
                self.next()
                e = self.parse_expr()
                v = self._const_eval_expr(e)
                if v is not None:
                    val = v
                else:
                    self.partial.append("non-constant enum value")
            self.constants[name] = val
            self.const_scopes[-1][name] = val
            next_val = val + 1
            if self.at_punct(","):
                self.next()
        self.expect_punct("}")

    def _const_eval_expr(self, e):
        """Evaluate a simple constant expression; None if not constant."""
        tag = e[0]
        if tag == "num":
            try:
                return int(e[1].rstrip("uUlLfF"), 0)
            except ValueError:
                return None
        if tag == "var":
            name = e[1].lstrip("?")
            for scope in reversed(self.const_scopes):
                if name in scope:
                    return scope[name]
            return self.numeric_macros.get(name)
        if tag == "unary":
            v = self._const_eval_expr(e[2])
            if v is None:
                return None
            op = e[1]
            if op == "-":
                return -v
            if op == "+":
                return v
            if op == "~":
                return ~v
            if op == "!":
                return int(not v)
            return None
        if tag == "binary":
            l = self._const_eval_expr(e[2])
            r = self._const_eval_expr(e[3])
            if l is None or r is None:
                return None
            op = e[1]
            try:
                if op == "+":
                    return l + r
                if op == "-":
                    return l - r
                if op == "*":
                    return l * r
                if op == "/":
                    return l // r if r else None
                if op == "%":
                    return l % r if r else None
                if op == "<<":
                    return l << r
                if op == ">>":
                    return l >> r
                if op == "&":
                    return l & r
                if op == "|":
                    return l | r
                if op == "^":
                    return l ^ r
                if op == "==":
                    return int(l == r)
                if op == "!=":
                    return int(l != r)
                if op == "<":
                    return int(l < r)
                if op == "<=":
                    return int(l <= r)
                if op == ">":
                    return int(l > r)
                if op == ">=":
                    return int(l >= r)
                if op == "&&":
                    return int(bool(l) and bool(r))
                if op == "||":
                    return int(bool(l) or bool(r))
            except (OverflowError, ZeroDivisionError):
                return None
            return None
        if tag == "cond":
            c = self._const_eval_expr(e[1])
            if c is None:
                return None
            return self._const_eval_expr(e[2] if c else e[3])
        if tag == "cast":
            return self._const_eval_expr(e[2])
        if tag == "sizeof":
            return self._const_sizeof(e[1])
        return None

    @staticmethod
    def _string_literal_size(text):
        """Bytes in a (possibly concatenated) narrow string literal,
        including the terminating NUL."""
        total = 0
        saw_quote = False
        i = 0
        n = len(text)
        simple = {"a": 1, "b": 1, "f": 1, "n": 1, "r": 1, "t": 1,
                  "v": 1, "\\": 1, "'": 1, '"': 1, "?": 1}
        while i < n:
            if text[i] != '"':
                i += 1
                continue
            saw_quote = True
            i += 1
            while i < n:
                c = text[i]
                if c == "\\":
                    if i + 1 >= n:
                        i = n
                        break
                    esc = text[i + 1]
                    if esc == "x":
                        j = i + 2
                        while j < n and text[j] in "0123456789abcdefABCDEF":
                            j += 1
                        total += 1
                        i = j
                        continue
                    total += simple.get(esc, 1)
                    i += 2
                    continue
                if c == '"':
                    i += 1
                    break
                total += 1
                i += 1
        return total + 1 if saw_quote else None

    def _const_sizeof(self, operand):
        if operand is None:
            return None
        if operand[0] == "str":
            return self._string_literal_size(operand[1])
        if operand[0] == "var":
            name = operand[1]
            return self.object_sizes.get(name,
                                         self.object_sizes.get(name.lstrip("?")))
        return None

    def _record_object_size(self, uniq, ptr_depth, dims, init):
        """Record sizeof() for narrow char arrays with string initializers.

        This covers the common ``static const char s[] = "..."`` pattern
        used for constant keys/guids without pretending to know arbitrary
        struct or non-char element sizes.
        """
        if ptr_depth or not isinstance(init, tuple) or init[0] != "str" \
                or not init[1].startswith('"'):
            return
        strlen = self._string_literal_size(init[1])
        if strlen is None:
            return
        if not dims:
            self.object_sizes[uniq] = strlen
            return
        if all(d is None for d in dims):
            self.object_sizes[uniq] = strlen
            return
        if all(d is not None and d[0] == "num" for d in dims):
            size = 1
            for d in dims:
                try:
                    size *= int(d[1].rstrip("uUlLfF"), 0)
                except ValueError:
                    return
            self.object_sizes[uniq] = size

    def unique_name(self, name):
        self.uid += 1
        base = f"{name}#{self.uid}"
        for scope in reversed(self.scope):
            if name in scope:
                # shadowing: still unique per declaration site
                pass
        return base

    def declare(self, name):
        uniq = self.unique_name(name)
        self.scope[-1][name] = uniq
        return uniq

    def _refresh_visible_typedefs(self):
        names = set(self.typedef_names)
        for scope in self.typedef_scopes:
            names.update(scope)
        self.visible_typedef_names = names

    def _declare_typedef(self, name):
        self.typedef_scopes[-1].add(name)
        self._refresh_visible_typedefs()

    def lookup(self, name):
        for scope in reversed(self.scope):
            if name in scope:
                return scope[name]
        # undeclared (shouldn't happen in valid C); make stable pseudo-name
        return f"?{name}"

    def _enum_constant_value(self, name):
        """Value of an enum constant, respecting block-scope shadowing."""
        for level in range(len(self.const_scopes) - 1, -1, -1):
            if level < len(self.scope) and name in self.scope[level]:
                return None
            if name in self.const_scopes[level]:
                return self.const_scopes[level][name]
        return None

    def _parse_array_dim(self):
        """Parse '[bound]' after consuming nothing; expects '[' current.
        Returns (dim_expr_or_None, is_vla)."""
        self.next()  # '['
        if self.at_punct("]"):
            self.next()
            return None, False
        e = self.parse_expr()
        cv = self._const_eval_expr(e)
        if cv is not None:
            e = ("num", str(cv), e[-1])
        self.expect_punct("]")
        return e, cv is None

    def _parse_declarator_core(self):
        """Parse a declarator without initializer.

        Returns (name, ptr_depth, dims, is_funcptr, is_function, vla).
        Parenthesized declarators are handled recursively so forms such as
        ``int (x)``, ``int (*p)[4]`` and ``int (*(*pp))(void)`` do not need
        a special-case funcptr parser.
        """
        ptr = 0
        while self.at_punct("*"):
            ptr += 1
            self.next()
            while self.peek().text in QUALIFIER_KWS:
                self.next()

        dims = []
        is_funcptr = False
        is_function = False
        vla = False

        def add_suffixes(after_paren=False, inner_ptr=0):
            nonlocal is_funcptr, is_function, vla
            while True:
                self.skip_attribute()
                if self.at_punct("["):
                    if after_paren and inner_ptr > 0:
                        # pointer to array: the bound describes the pointee,
                        # not the pointer variable itself
                        is_funcptr = True
                        self.skip_balanced("[", "]")
                        continue
                    dim, dim_vla = self._parse_array_dim()
                    dims.append(dim)
                    vla = vla or dim_vla
                    continue
                if self.at_punct("("):
                    if after_paren and inner_ptr > 0:
                        is_funcptr = True
                    else:
                        is_function = True
                    self.skip_balanced("(", ")")
                    self.skip_attribute()
                    continue
                break

        if self.at_punct("("):
            self.next()  # '('
            name, iptr, idims, ifuncptr, ifunction, ivla = \
                self._parse_declarator_core()
            self.expect_punct(")")
            ptr += iptr
            dims.extend(idims)
            is_funcptr = is_funcptr or ifuncptr
            is_function = is_function or ifunction
            vla = vla or ivla
            add_suffixes(after_paren=True, inner_ptr=iptr)
            return name, ptr, dims, is_funcptr, is_function, vla

        if self.peek().kind != "id":
            raise ParseError(f"expected declarator name at line {self.peek().line}")
        name = self.next().text
        self.skip_attribute()
        add_suffixes(after_paren=False)
        return name, ptr, dims, is_funcptr, is_function, vla

    def parse_declarator(self, in_for=False, declare_var=True,
                         type_size=None):
        """Parse one declarator. Returns VarDecl."""
        line = self.peek().line
        name, ptr, dims, is_funcptr, is_function, vla = \
            self._parse_declarator_core()
        self.skip_attribute()
        init = None
        complex_init = False
        if self.at_punct("="):
            self.next()
            if self.at_punct("{"):
                self.skip_balanced("{", "}")
                complex_init = True
            else:
                init = self.parse_expr()
        should_declare = declare_var and not is_function
        uniq = self.declare(name) if should_declare else name
        if should_declare:
            self._record_object_size(uniq, ptr, dims, init)
        if vla:
            self.partial.append("variably-modified array type")
        return VarDecl(uniq, ptr, dims, "complex" if complex_init else init,
                       is_funcptr, line, vla, is_function, type_size)

    def parse_decl(self, for_mode=False):
        """Parse a declaration; returns list of VarDecl.
        In for_mode the declaration ends at ')' instead of ';'."""
        self._decl_typedef = False
        spec_start = self.i
        complete = self.skip_specifiers()
        if complete:
            return []
        spec_texts = [t.text for t in self.toks[spec_start:self.i]
                      if t.kind in ("kw", "id")]
        type_size = decl_type_size(spec_texts)
        is_typedef = self._decl_typedef
        decls = []
        while True:
            d = self.parse_declarator(declare_var=not is_typedef,
                                      type_size=type_size)
            if is_typedef:
                self._declare_typedef(d.name)
            elif not d.is_function:
                decls.append(d)
            if self.at_punct(","):
                self.next()
                continue
            break
        return [] if is_typedef else decls

    # ---- preprocessor filtering ----------------------------------------
    def _pp_tokens(self, text):
        return tokenize(text)

    def _pp_eval_expr(self, tokens, defines, unknown_ok=False):
        """Evaluate a bounded integer preprocessor expression.

        Returns an int, or None when ``unknown_ok`` is true and the result
        depends on an identifier without a numeric value. ``&&`` and ``||``
        short-circuit so ``defined(X) && X > 0`` does not require X.
        """
        tokens = [t for t in tokens if t.kind != "eof"]
        pos = 0
        n = len(tokens)

        def fail(msg="bad #if expression"):
            if unknown_ok:
                return None
            raise ParseError(msg)

        def peek(k=0):
            j = pos + k
            return tokens[j] if j < n else None

        def take():
            nonlocal pos
            t = peek()
            if t is not None:
                pos += 1
            return t

        def at_punct(s):
            t = peek()
            return t is not None and t.kind == "punct" and t.text == s

        def match_paren(start):
            depth = 0
            for k in range(start, n):
                t = tokens[k]
                if t.kind == "punct":
                    if t.text == "(":
                        depth += 1
                    elif t.text == ")":
                        depth -= 1
                        if depth == 0:
                            return k
            return None

        def is_type_seq(a, b):
            """True if tokens[a:b] look like a C type name for a cast."""
            saw_type = False
            k = a
            while k < b:
                t = tokens[k]
                if t.kind == "kw":
                    if t.text in ("struct", "union", "enum"):
                        saw_type = True
                        k += 1
                        if k < b and tokens[k].kind == "id":
                            k += 1
                        continue
                    if t.text in SPEC_KWS and t.text not in (
                            "_Alignof", "_Static_assert", "_Noreturn",
                            "_Thread_local", "typedef", "inline", "extern",
                            "auto", "register"):
                        saw_type = True
                        k += 1
                        continue
                    return False
                if t.kind == "id":
                    text = t.text
                    if text in self.visible_typedef_names \
                            or text in BUILTIN_TYPE_NAMES \
                            or text.endswith("_t") \
                            or text.startswith("cfw_"):
                        saw_type = True
                        k += 1
                        continue
                    return False
                if t.kind == "punct" and t.text in ("*", "(", ")", "[",
                                                    "]"):
                    k += 1
                    continue
                return False
            return saw_type

        def cast_end(start):
            """If '(' at start opens a cast, return index of its ')' ."""
            end = match_paren(start)
            if end is None or end + 1 >= n or not is_type_seq(start + 1,
                                                              end):
                return None
            nt = tokens[end + 1]
            if nt.kind in ("num", "id", "kw") \
                    or (nt.kind == "punct" and nt.text in (
                        "(", "!", "~", "-", "+", "*", "&", "++", "--")):
                return end
            return None

        def primary():
            t = peek()
            if t is None:
                return fail("unexpected end of #if")
            if t.kind == "num":
                take()
                try:
                    return int(t.text.rstrip("uUlLfF"), 0)
                except ValueError:
                    return fail(f"bad #if number {t.text!r}")
            if t.kind == "punct" and t.text == "(":
                take()
                v = parse_or()
                if not at_punct(")"):
                    return fail("missing ')' in #if")
                take()
                return v
            if t.kind == "id":
                take()
                if t.text in INTEGER_CONSTANT_MACROS and at_punct("("):
                    take()
                    v = parse_or()
                    if not at_punct(")"):
                        return fail("missing ')' after integer constant "
                                    "macro")
                    take()
                    if v is None:
                        return fail("non-constant integer constant macro")
                    return v
                if t.text in defines:
                    v = defines[t.text]
                    if isinstance(v, int):
                        return v
                    return fail(f"#if identifier {t.text!r} has no "
                                "numeric value")
                return fail(f"unknown #if identifier {t.text!r}")
            return fail(f"unsupported #if token {t.text!r}")

        def unary():
            t = peek()
            if t is None:
                return fail("unexpected end of #if")
            if at_punct("("):
                end = cast_end(pos)
                if end is not None:
                    while pos <= end:
                        take()
                    return unary()
            if t.kind == "punct" and t.text in ("!", "~", "-", "+"):
                op = take().text
                v = unary()
                if v is None:
                    return None
                if op == "!":
                    return int(not v)
                if op == "~":
                    return ~v
                if op == "-":
                    return -v
                return v
            if t.kind == "id" and t.text == "defined":
                take()
                name = None
                if at_punct("("):
                    take()
                    nt = peek()
                    if nt is None or nt.kind != "id":
                        return fail("bad defined() in #if")
                    name = take().text
                    if not at_punct(")"):
                        return fail("missing ')' after defined")
                    take()
                else:
                    nt = peek()
                    if nt is None or nt.kind != "id":
                        return fail("bad defined in #if")
                    name = take().text
                return int(name in defines)
            return primary()

        def binary(next_level, ops, apply):
            left = next_level()
            while True:
                t = peek()
                if t is None or t.kind != "punct" or t.text not in ops:
                    break
                op = take().text
                right = next_level()
                left = apply(op, left, right)
            return left

        def arith(op, left, right):
            if left is None or right is None:
                return None
            try:
                if op == "*":
                    return left * right
                if op == "/":
                    return left // right
                if op == "%":
                    return left % right
                if op == "+":
                    return left + right
                if op == "-":
                    return left - right
                if op == "<<":
                    return left << right
                if op == ">>":
                    return left >> right
            except (OverflowError, ZeroDivisionError):
                return fail("bad #if arithmetic")
            return fail(f"unsupported #if operator {op!r}")

        def bitwise(op, left, right):
            if left is None or right is None:
                return None
            if op == "&":
                return left & right
            if op == "^":
                return left ^ right
            if op == "|":
                return left | right
            return fail(f"unsupported #if operator {op!r}")

        def equality(op, left, right):
            if left is None or right is None:
                return None
            if op == "==":
                return int(left == right)
            if op == "!=":
                return int(left != right)
            return fail(f"unsupported #if operator {op!r}")

        def relational(op, left, right):
            if left is None or right is None:
                return None
            if op == "<":
                return int(left < right)
            if op == "<=":
                return int(left <= right)
            if op == ">":
                return int(left > right)
            if op == ">=":
                return int(left >= right)
            return fail(f"unsupported #if operator {op!r}")

        def logic(op, left, right):
            if op == "&&":
                if left == 0:
                    return 0
                if right == 0:
                    return 0
                if left is None or right is None:
                    return None
                return int(bool(left) and bool(right))
            if op == "||":
                if left == 1:
                    return 1
                if right == 1:
                    return 1
                if left is None or right is None:
                    return None
                return int(bool(left) or bool(right))
            return fail(f"unsupported #if operator {op!r}")

        def parse_mul():
            return binary(unary, {"*", "/", "%"}, arith)

        def parse_add():
            return binary(parse_mul, {"+", "-"}, arith)

        def parse_shift():
            return binary(parse_add, {"<<", ">>"}, arith)

        def parse_rel():
            return binary(parse_shift, {"<", "<=", ">", ">="}, relational)

        def parse_eq():
            return binary(parse_rel, {"==", "!="}, equality)

        def parse_and_bits():
            return binary(parse_eq, {"&"}, bitwise)

        def parse_xor():
            return binary(parse_and_bits, {"^"}, bitwise)

        def parse_or_bits():
            return binary(parse_xor, {"|"}, bitwise)

        def parse_and():
            return binary(parse_or_bits, {"&&"}, logic)

        def parse_or():
            return binary(parse_and, {"||"}, logic)

        result = parse_or()
        if pos != n:
            t = tokens[pos]
            return fail(f"unsupported #if token {t.text!r}")
        return result

    def _pp_filter(self, tokens, defines):
        """Drop inactive #if/#ifdef/#ifndef branches and all pp tokens.

        Unknown conditional expressions keep all branches and are reported
        as partial by the caller; known platform/configuration branches are
        resolved conservatively so ordinary platform #if bodies do not make
        every function partial. Stack frames are
        ``[local_active, taken, unknown]`` for the current #if group.
        """
        out = []
        unknown_lines = []
        stack = []

        def active():
            return all(x[0] for x in stack)

        def outer_active():
            return all(x[0] for x in stack[:-1])

        for t in tokens:
            if t.kind != "pp":
                if active():
                    out.append(t)
                continue
            text = t.text.strip()
            if re.match(r"#if\b", text):
                expr = text[3:].strip()
                if not active():
                    stack.append([False, False, False])
                    continue
                try:
                    val = self._pp_eval_expr(self._pp_tokens(expr),
                                             defines, unknown_ok=True)
                    cond = bool(val) if val is not None else None
                except ParseError:
                    cond = None
                if cond is None:
                    unknown_lines.append(t.line)
                stack.append([cond is not False, cond is True, cond is None])
            elif text.startswith("#ifdef"):
                name = text[6:].strip().split()[0] if text[6:].strip() else ""
                cond = (name in defines) if active() else False
                stack.append([cond, cond, False])
            elif text.startswith("#ifndef"):
                name = text[7:].strip().split()[0] if text[7:].strip() else ""
                cond = (name not in defines) if active() else False
                stack.append([cond, cond, False])
            elif text.startswith("#elif"):
                if not stack:
                    continue
                frame = stack[-1]
                if frame[1] or not outer_active():
                    frame[0] = False
                    continue
                expr = text[5:].strip()
                try:
                    val = self._pp_eval_expr(self._pp_tokens(expr),
                                             defines, unknown_ok=True)
                    cond = bool(val) if val is not None else None
                except ParseError:
                    cond = None
                if cond is None:
                    unknown_lines.append(t.line)
                frame[0] = cond is not False
                frame[1] = frame[1] or cond is True
                frame[2] = frame[2] or cond is None
            elif text.startswith("#else"):
                if stack:
                    frame = stack[-1]
                    frame[0] = outer_active() and not frame[1]
                    frame[1] = True
            elif text.startswith("#endif"):
                if stack:
                    stack.pop()
            # Other directives (#define/#include/#pragma/...) are dropped.
        return out, unknown_lines

    # ---- expressions ---------------------------------------------------
    def looks_like_type_tokens(self, start):
        """Scan tokens[start..matching ')' of the '(' at start]; return True
        if contents look like a type name sequence."""
        depth = 0
        j = start
        n = len(self.toks)
        saw_type_kw = False
        saw_id = False
        all_ids_type_like = True
        while j < n:
            t = self.toks[j]
            if t.kind == "punct":
                if t.text == "(":
                    # a nested '(' inside means this is a parenthesized
                    # expression like ((T*)x), not a type name
                    if depth >= 1:
                        return False
                    depth += 1
                elif t.text == "[":
                    depth += 1
                elif t.text == ")":
                    depth -= 1
                    if depth == 0:
                        break
                elif t.text == "]":
                    depth -= 1
                elif t.text == "*":
                    pass  # pointer in type name
                else:
                    # any other punctuator (|, &, &&, ,, ->, ...) means
                    # this is an expression, not a type name
                    return False
            elif t.kind == "kw":
                if t.text in SPEC_KWS or t.text in ("const", "volatile",
                                                  "restrict"):
                    if t.text in ("void", "char", "short", "int", "long",
                                  "float", "double", "signed", "unsigned",
                                  "_Bool", "_Complex", "struct", "union",
                                  "enum", "const", "volatile", "restrict"):
                        saw_type_kw = True
                    # other spec kw (typedef/inline/...) inside a cast is
                    # unusual; don't disqualify
                else:
                    return False
            elif t.kind == "id":
                saw_id = True
                if not (t.text in self.visible_typedef_names
                        or t.text in BUILTIN_TYPE_NAMES
                        or t.text.endswith("_t")
                        or t.text.startswith("cfw_")
                        or t.text.isupper()):
                    all_ids_type_like = False
            elif t.kind in ("num", "str", "chr"):
                return False
            elif t.kind == "pp":
                return False
            j += 1
        if j >= n or self.toks[j].kind != "punct" or self.toks[j].text != ")":
            return False
        if saw_type_kw or self._inner_has_type_kw(start, j):
            return True
        # '(type_name)' casts where the type is a typedef/known name with
        # no type keyword, e.g. (intptr_t)x or (cfw_foo_t *)p. Require at
        # least one id and every id inside to look like a type name so
        # plain parenthesized expressions stay expressions.
        return saw_id and all_ids_type_like

    def _inner_has_type_kw(self, a, b):
        for k in range(a, b):
            t = self.toks[k]
            if t.kind == "kw" and t.text in (
                    "void", "char", "short", "int", "long", "float",
                    "double", "signed", "unsigned", "_Bool", "struct",
                    "union", "enum", "const", "volatile", "restrict"):
                return True
        return False

    def skip_attribute(self):
        while (self.peek().kind == "id"
               and self.peek().text in ("__attribute__", "__declspec")):
            self.next()
            if self.at_punct("("):
                self.skip_balanced("(", ")")

    def parse_expr(self, min_prec=0):
        return self._parse_binary(min_prec)

    def parse_comma_expr(self):
        """Parse a full expression including top-level comma operators.
        Use only where ',' is not a separator (conditions, expression
        statements, return values) -- NOT call args or initializers."""
        e = self.parse_expr()
        if not self.at_punct(","):
            return e
        exprs = [e]
        line = e[-1] if isinstance(e, tuple) else 0
        while self.at_punct(","):
            self.next()
            exprs.append(self.parse_expr())
        return ("comma", exprs, line)

    def _parse_binary(self, min_prec):
        if self.peek().kind == "punct" and self.peek().text == "?":
            raise ParseError("unexpected ?")
        # ternary handled at prec 2 level via special-case below
        left = self._parse_unary()
        while True:
            t = self.peek()
            if t.kind == "punct" and t.text == "?":
                if 2 < min_prec:
                    break
                self.next()
                th = self.parse_expr()
                self.expect_punct(":")
                el = self.parse_expr(2)
                left = ("cond", left, th, el, t.line)
                continue
            if t.kind != "punct":
                break
            op = t.text
            if op in ASSIGN_OPS:
                prec = 1
            elif op in BIN_PREC:
                prec = BIN_PREC[op]
            else:
                break
            if prec < min_prec:
                break
            if op == "&&" or op == "||":
                # still binary node; refinement done by analyzer
                pass
            self.next()
            right = self.parse_expr(prec + (0 if op in RIGHT_ASSOC else 1))
            if op in ASSIGN_OPS:
                left = ("assign", op, left, right, t.line)
            else:
                left = ("binary", op, left, right, t.line)
        return left

    def _parse_unary(self):
        t = self.peek()
        if t.kind == "pp":
            self.skip_pp()
            t = self.peek()
        if t.kind == "punct":
            if t.text == "&&" and self.peek(1).kind == "id":
                # computed-goto label address (&&label): unsupported value,
                # but parse it so the rest of the function stays analyzable.
                self.partial.append("computed goto")
                self.next()
                self.next()
                return ("unknown", t.line)
            if t.text in ("!", "~", "-", "+", "*", "&", "++", "--"):
                self.next()
                e = self._parse_unary()
                if t.text == "&":
                    return ("addr", e, t.line)
                return ("unary", t.text, e, t.line)
            if t.text == "(":
                if self.peek(1).kind == "punct" \
                        and self.peek(1).text == "{":
                    # GNU statement expression: keep the function partial
                    # but do not let one construct opaque the whole body.
                    self.partial.append("statement expression")
                    self.skip_balanced("(", ")")
                    return self._postfix_loop(("unknown", t.line))
                # cast or parenthesized expression
                if self.looks_like_type_tokens(self.i):
                    # collect type tokens until matching ')'
                    type_toks = []
                    depth = 0
                    j = self.i
                    while True:
                        tt = self.toks[j]
                        if tt.kind == "punct":
                            if tt.text in "([{":
                                depth += 1
                            elif tt.text in ")]}":
                                depth -= 1
                                if depth == 0:
                                    break
                        type_toks.append(tt.text)
                        j += 1
                    type_str = " ".join(type_toks[1:-1])
                    # consume through ')'
                    self.i = j + 1
                    if self.at_punct("{"):
                        # compound literal: (type){init}
                        self.skip_balanced("{", "}")
                        return ("compound", type_str, t.line)
                    operand = self._parse_unary()
                    return self._postfix_loop(
                        ("cast", type_str, operand, t.line))
                self.next()
                e = self.parse_comma_expr()
                self.expect_punct(")")
                return self._postfix_loop(e)
            if t.text == "sizeof":
                pass
        if t.kind == "kw" and t.text == "sizeof":
            self.next()
            if self.at_punct("(") and self.looks_like_type_tokens(self.i):
                depth = 0
                while True:
                    tt = self.next()
                    if tt.kind == "punct":
                        if tt.text in "([{":
                            depth += 1
                        elif tt.text in ")]}":
                            depth -= 1
                            if depth == 0:
                                break
                return ("sizeof", None, t.line)
            e = self._parse_unary()
            return ("sizeof", e, t.line)
        if t.kind == "kw" and t.text in ("_Alignof",):
            self.next()
            if self.at_punct("("):
                self.skip_balanced("(", ")")
            else:
                self._parse_unary()
            return ("unknown", t.line)
        if t.kind == "kw" and t.text in ("_Generic",):
            self.partial.append("_Generic expression")
            self.next()
            # skip balanced parens best-effort
            if self.at_punct("("):
                self.skip_balanced("(", ")")
            return self._postfix_loop(("unknown", t.line))
        if t.kind == "id" and t.text in ("asm", "__asm", "__asm__"):
            # asm is not a C keyword; only treat it as inline assembly when
            # an assembler template '(' follows optional qualifiers.
            j = self.i + 1
            n = len(self.toks)
            while j < n:
                tt = self.toks[j]
                if tt.kind == "kw" and tt.text in ("volatile", "goto",
                                                   "inline"):
                    j += 1
                    continue
                if tt.kind == "id" and tt.text == "__volatile__":
                    j += 1
                    continue
                break
            if j + 1 < n and self.toks[j].kind == "punct" \
                    and self.toks[j].text == "(" \
                    and self.toks[j + 1].kind == "str":
                self.partial.append("inline assembly")
                self.i = j
                self.skip_balanced("(", ")")
                # Skip asm goto output/input label lists up to ';'.
                while self.peek().kind != "eof" \
                        and not self.at_punct(";"):
                    self.next()
                return self._postfix_loop(("unknown", t.line))
        return self._postfix_loop(self._parse_primary())

    def _is_va_arg_expr(self, e):
        return e[0] == "var" and e[1].lstrip("?") in VA_ARG_NAMES

    def _parse_va_arg_call(self, e, line):
        """Parse va_arg(ap, type-name); the second operand is a type,
        not an expression, so it cannot go through parse_expr."""
        first = self.parse_expr()
        self.expect_punct(",")
        type_toks = []
        depth = 0
        while True:
            t = self.next()
            if t.kind == "eof":
                raise ParseError("unterminated va_arg type")
            if t.kind == "punct":
                if t.text == "(":
                    depth += 1
                elif t.text == ")":
                    if depth == 0:
                        break
                    depth -= 1
                elif t.text == "," and depth == 0:
                    raise ParseError("bad va_arg argument list")
            type_toks.append(t.text)
        return ("va_arg", first, " ".join(type_toks).strip(), line)

    def _postfix_loop(self, e):
        while True:
            t = self.peek()
            if t.kind == "punct":
                if t.text == "(":
                    self.next()
                    if self._is_va_arg_expr(e):
                        e = self._parse_va_arg_call(e, t.line)
                        continue
                    args = []
                    if not self.at_punct(")"):
                        while True:
                            args.append(self.parse_expr())
                            if self.at_punct(","):
                                self.next()
                                continue
                            break
                    self.expect_punct(")")
                    e = ("call", e, args, t.line)
                    continue
                if t.text == "[":
                    self.next()
                    idx = self.parse_expr()
                    self.expect_punct("]")
                    e = ("index", e, idx, t.line)
                    continue
                if t.text == ".":
                    self.next()
                    if self.peek().kind != "id":
                        raise ParseError("expected member name")
                    name = self.next().text
                    e = ("member", e, name, False, t.line)
                    continue
                if t.text == "->":
                    self.next()
                    if self.peek().kind != "id":
                        raise ParseError("expected member name")
                    name = self.next().text
                    e = ("member", e, name, True, t.line)
                    continue
                if t.text in ("++", "--"):
                    self.next()
                    e = ("postfix", t.text, e, t.line)
                    continue
            break
        return e

    def _parse_primary(self):
        t = self.next()
        if t.kind == "num":
            return ("num", t.text, t.line)

        def string_literal(first_text, line):
            text = first_text
            while True:
                while self.peek().kind == "str":
                    text += self.next().text
                nt = self.peek()
                if nt.kind == "id" and nt.text in self.string_macros:
                    self.next()
                    text += self.string_macros[nt.text]
                    continue
                break
            return ("str", text, line)

        if t.kind == "str":
            # adjacent string literals concatenate in C
            return string_literal(t.text, t.line)
        if t.kind == "chr":
            return ("chr", t.text, t.line)
        if t.kind == "id":
            if t.text in INTEGER_CONSTANT_MACROS and self.at_punct("("):
                self.next()  # '('
                arg = self.parse_expr()
                self.expect_punct(")")
                v = self._const_eval_expr(arg)
                if v is not None:
                    return ("num", str(v), t.line)
                return ("unknown", t.line)
            enum_v = self._enum_constant_value(t.text)
            if enum_v is not None:
                return ("num", str(enum_v), t.line)
            if t.text in self.string_macros:
                return string_literal(self.string_macros[t.text], t.line)
            if t.text in self.numeric_macros:
                return ("num", str(self.numeric_macros[t.text]), t.line)
            return ("var", self.lookup(t.text), t.line)
        if t.kind == "kw" and t.text in ("true", "false"):
            return ("num", "1" if t.text == "true" else "0", t.line)
        if t.kind == "punct" and t.text == "(":
            # parenthesized expr that wasn't a cast (shouldn't reach here
            # often because _parse_unary handles '('), but be safe
            e = self.parse_comma_expr()
            self.expect_punct(")")
            return e
        if t.kind == "pp":
            self.partial.append("preprocessor line inside expression")
            return ("unknown", t.line)
        raise ParseError(f"unexpected token '{t.text}' ({t.kind}) "
                         f"at line {t.line}")

    # ---- statements ----------------------------------------------------
    def parse_stmt(self):
        self.skip_pp()
        t = self.peek()
        if t.kind == "punct":
            if t.text == "{":
                return self.parse_block()
            if t.text == ";":
                self.next()
                return ("empty", t.line)
        if t.kind == "kw":
            k = t.text
            if k == "_Static_assert":
                self.next()
                if self.at_punct("("):
                    self.skip_balanced("(", ")")
                self.expect_punct(";")
                return ("empty", t.line)
            if k == "if":
                return self.parse_if()
            if k == "while":
                return self.parse_while()
            if k == "do":
                return self.parse_do()
            if k == "for":
                return self.parse_for()
            if k == "switch":
                return self.parse_switch()
            if k == "return":
                self.next()
                e = None
                if not self.at_punct(";"):
                    e = self.parse_comma_expr()
                self.expect_punct(";")
                return ("return", e, t.line)
            if k in ("break", "continue"):
                self.next()
                self.expect_punct(";")
                return (k, t.line)
            if k == "goto":
                self.next()
                if self.at_punct("*"):
                    self.partial.append("computed goto")
                    self.next()
                    self.parse_expr()
                    self.expect_punct(";")
                    return ("opaque", t.line)
                name = self.next()
                if name.kind != "id":
                    raise ParseError(f"expected goto label at {name.line}")
                self.expect_punct(";")
                return ("goto", name.text, t.line)
            if k == "case":
                self.next()
                e = self.parse_expr()
                self.expect_punct(":")
                return ("case", e, t.line)
            if k == "default":
                self.next()
                self.expect_punct(":")
                return ("default", t.line)
            if k == "else":
                raise ParseError("stray else")
        if self.is_decl_start():
            decls = self.parse_decl()
            if not decls:
                # complete type/enum declaration with no declarator
                self.expect_punct(";")
                return ("empty", t.line)
            self.expect_punct(";")
            return ("decl", decls, t.line)
        if t.kind == "id" and self.peek(1).kind == "punct" \
                and self.peek(1).text == ":":
            self.next()
            self.next()
            name = t.text
            if name in self.label_seen:
                self.partial.append(f"duplicate label {name}")
            self.label_seen.add(name)
            return ("label", name, t.line)
        # expression statement
        e = self.parse_comma_expr()
        self.expect_punct(";")
        return ("expr", e, t.line)

    def parse_block(self):
        t = self.expect_punct("{")
        self.scope.append({})
        self.const_scopes.append({})
        self.typedef_scopes.append(set())
        self._refresh_visible_typedefs()
        stmts = []
        try:
            while True:
                self.skip_pp()
                if self.at_punct("}"):
                    break
                if self.peek().kind == "eof":
                    raise ParseError("unterminated block")
                stmts.append(self.parse_stmt())
        finally:
            self.scope.pop()
            self.const_scopes.pop()
            self.typedef_scopes.pop()
            self._refresh_visible_typedefs()
        self.expect_punct("}")
        return ("block", stmts, t.line)

    def parse_paren_expr(self):
        self.expect_punct("(")
        if self.at_punct(")"):
            self.next()
            return None
        e = self.parse_comma_expr()
        self.expect_punct(")")
        return e

    def parse_if(self):
        t = self.next()  # if
        cond = self.parse_paren_expr()
        then = self.parse_stmt()
        els = None
        if self.at_kw("else"):
            self.next()
            els = self.parse_stmt()
        return ("if", cond, then, els, t.line)

    def parse_while(self):
        t = self.next()
        cond = self.parse_paren_expr()
        body = self.parse_stmt()
        return ("while", cond, body, t.line)

    def parse_do(self):
        t = self.next()
        body = self.parse_stmt()
        if not self.at_kw("while"):
            raise ParseError("do without while")
        self.next()
        cond = self.parse_paren_expr()
        self.expect_punct(";")
        return ("do", body, cond, t.line)

    def parse_for(self):
        t = self.next()
        self.expect_punct("(")
        init_decls = None
        init_exprs = None
        cond = None
        incr = None
        if self.at_punct(";"):
            self.next()
        elif self.is_decl_start():
            self.skip_specifiers()
            init_decls = self.parse_decl(for_mode=True)
            self.expect_punct(";")
        else:
            init_exprs = [self.parse_expr()]
            while self.at_punct(","):
                self.next()
                init_exprs.append(self.parse_expr())
            self.expect_punct(";")
        if not self.at_punct(";"):
            cond = self.parse_comma_expr()
        self.expect_punct(";")
        if not self.at_punct(")"):
            incr = [self.parse_expr()]
            while self.at_punct(","):
                self.next()
                incr.append(self.parse_expr())
        self.expect_punct(")")
        body = self.parse_stmt()
        return ("for", init_decls, init_exprs, cond, incr, body, t.line)

    def parse_switch(self):
        t = self.next()
        expr = self.parse_paren_expr()
        body = self.parse_stmt()
        return ("switch", expr, body, t.line)

    # ---- function header ----------------------------------------------
    def parse_params(self, lparen_idx):
        """Parse parameter list tokens between matching parens at index."""
        # tokens[lparen_idx] == '('
        depth = 0
        j = lparen_idx
        n = len(self.toks)
        while j < n:
            t = self.toks[j]
            if t.kind == "punct":
                if t.text == "(":
                    depth += 1
                elif t.text == ")":
                    depth -= 1
                    if depth == 0:
                        break
            j += 1
        if j >= n:
            raise ParseError("unbalanced params")
        inner = self.toks[lparen_idx + 1:j]
        # split on top-level commas
        parts = []
        depth = 0
        cur = []
        for t in inner:
            if t.kind == "punct":
                if t.text in "([{":
                    depth += 1
                elif t.text in ")]}":
                    depth -= 1
                elif t.text == "," and depth == 0:
                    parts.append(cur)
                    cur = []
                    continue
            cur.append(t)
        if cur:
            parts.append(cur)
        params = []
        for p in parts:
            texts = [t.text for t in p]
            if texts == ["void"] or texts == ["..."]:
                continue
            if all(t.kind == "pp" for t in p):
                continue
            # param name = last id at depth 0 not preceded by '.' etc.
            # scan for last identifier
            name = None
            d = 0
            for t in p:
                if t.kind == "punct":
                    if t.text in "([{":
                        d += 1
                    elif t.text in ")]}":
                        d -= 1
                elif t.kind == "id" and d == 0:
                    name = t.text
            if name is None:
                # maybe inside (*name) funcptr param
                for t in p:
                    if t.kind == "id":
                        name = t.text
            if name is None:
                # unnamed parameter (e.g. funcptr type without name);
                # keep parsing with a placeholder
                self.partial.append("unnamed parameter")
                name = f"?param{len(params)}"
            # pointer depth and array-ness of the parameter
            ptr_depth = 0
            has_dims = False
            d = 0
            for t in p:
                if t.kind == "punct":
                    if t.text in "([{":
                        d += 1
                    elif t.text in ")]}":
                        d -= 1
                    elif t.text == "*" and d == 0:
                        ptr_depth += 1
                    elif t.text == "[" and d == 0:
                        has_dims = True
            params.append((name, ptr_depth, has_dims))
        return params, j + 1  # index after ')'


def _header_is_function(header_toks):
    """Decide whether top-level tokens before '{' form a function header.
    Returns (name, lparen_index_in_header) or None."""
    # find candidate '(' groups from the right
    # tokens: list; find last ')' then match '('
    n = len(header_toks)
    # strip trailing attribute-ish tokens is handled by scanning all ')'
    for idx in range(n - 1, -1, -1):
        t = header_toks[idx]
        if t.kind == "punct" and t.text == ")":
            # match back to '('
            depth = 0
            j = idx
            while j >= 0:
                tt = header_toks[j]
                if tt.kind == "punct":
                    if tt.text == ")":
                        depth += 1
                    elif tt.text == "(":
                        depth -= 1
                        if depth == 0:
                            break
                j -= 1
            if j < 0:
                continue
            # token before '(' should be the function name (id)
            if j - 1 < 0:
                continue
            prev = header_toks[j - 1]
            if prev.kind != "id":
                continue
            if prev.text in ("__attribute__", "__declspec", "if", "while",
                             "for", "switch", "return", "sizeof"):
                continue
            # ensure nothing before looks like an expression: the tokens
            # before prev should be declaration specifiers only -- light
            # check: no '=' or ';' in header
            for tt in header_toks[: j - 1]:
                if tt.kind == "punct" and tt.text in ("=", ";"):
                    return None
            # also reject if prev is actually part of larger expr e.g.
            # 'x = f' can't happen since '=' filtered
            return prev.text, j
    return None


def _skip_balanced_tokens(toks, i, open_s, close_s):
    depth = 0
    n = len(toks)
    while i < n:
        t = toks[i]
        if t.kind == "punct":
            if t.text == open_s:
                depth += 1
            elif t.text == close_s:
                depth -= 1
                if depth == 0:
                    return i + 1
        i += 1
    return n


def _first_typedef_declarator_id(toks, start):
    """First identifier in a declarator, including ``(*name)`` groups.

    Parameter names inside a function-pointer parameter list are never
    reached because the declarator name precedes that list.
    """
    i = start
    n = len(toks)
    while i < n:
        t = toks[i]
        if t.kind == "id" and t.text not in ("__attribute__", "__declspec"):
            return i
        if t.kind == "id" and t.text in ("__attribute__", "__declspec"):
            i += 1
            if i < n and toks[i].kind == "punct" and toks[i].text == "(":
                i = _skip_balanced_tokens(toks, i, "(", ")")
            continue
        i += 1
    return None


def _typedef_declared_names(toks, known_names):
    """Extract typedef'd names from declaration-specifier + declarators."""
    n = len(toks)
    i = 0
    seen_type = False

    def at_punct(idx, s):
        return idx < n and toks[idx].kind == "punct" and toks[idx].text == s

    while i < n:
        t = toks[i]
        if t.kind == "kw" and t.text in SPEC_KWS:
            i += 1
            if t.text in ("struct", "union", "enum"):
                if i < n and toks[i].kind == "id":
                    i += 1
                if at_punct(i, "{"):
                    i = _skip_balanced_tokens(toks, i, "{", "}")
                seen_type = True
            elif t.text in ("void", "char", "short", "int", "long",
                            "float", "double", "signed", "unsigned",
                            "_Bool", "_Complex", "_Imaginary"):
                seen_type = True
            elif t.text in ("_Atomic", "_Alignas") and at_punct(i, "("):
                i = _skip_balanced_tokens(toks, i, "(", ")")
                seen_type = True
            continue
        if (not seen_type and t.kind == "id"
                and (t.text in known_names or t.text in BUILTIN_TYPE_NAMES
                     or t.text.endswith("_t") or t.text.startswith("cfw_")
                     or t.text.isupper())):
            i += 1
            seen_type = True
            continue
        if t.kind == "id" and t.text in ("__attribute__", "__declspec"):
            i += 1
            if at_punct(i, "("):
                i = _skip_balanced_tokens(toks, i, "(", ")")
            continue
        break

    names = []
    while i < n:
        name_idx = _first_typedef_declarator_id(toks, i)
        if name_idx is None:
            break
        names.append(toks[name_idx].text)
        # Advance to the next top-level comma; commas inside function
        # pointer parameter lists and array bounds do not separate
        # typedef declarators.
        j = i
        paren = brack = 0
        while j < n:
            tt = toks[j]
            if tt.kind == "punct":
                if tt.text == "(":
                    paren += 1
                elif tt.text == ")":
                    paren -= 1
                elif tt.text == "[":
                    brack += 1
                elif tt.text == "]":
                    brack -= 1
                elif tt.text == "," and paren == 0 and brack == 0:
                    break
            j += 1
        i = j + 1 if j < n else n
    return names


def collect_typedefs(tokens):
    """First pass: gather typedef names from top-level items."""
    names = set()
    n = len(tokens)
    i = 0
    brace_depth = 0
    while i < n:
        t = tokens[i]
        if t.kind == "pp":
            i += 1
            continue
        if t.kind == "punct":
            if t.text == "{":
                brace_depth += 1
            elif t.text == "}":
                brace_depth = max(0, brace_depth - 1)
        if (t.kind == "kw" and t.text == "typedef" and brace_depth == 0):
            j = i + 1
            paren = brack = brace = 0
            end = n
            while j < n:
                tt = tokens[j]
                if tt.kind == "pp":
                    j += 1
                    continue
                if tt.kind == "punct":
                    if tt.text == "(":
                        paren += 1
                    elif tt.text == ")":
                        paren -= 1
                    elif tt.text == "[":
                        brack += 1
                    elif tt.text == "]":
                        brack -= 1
                    elif tt.text == "{":
                        brace += 1
                    elif tt.text == "}":
                        brace -= 1
                    elif tt.text == ";" and paren == 0 and brack == 0 \
                            and brace == 0:
                        end = j
                        break
                j += 1
            if end < n:
                names.update(_typedef_declared_names(tokens[i + 1:end],
                                                     names))
                i = end + 1
                continue
        i += 1
    return names


def collect_enum_constants(tokens, typedef_names=frozenset(),
                           numeric_macros=None):
    """Collect file-scope enum constants with their integer values.

    Only ``enum`` bodies at brace depth zero are considered; constants
    declared inside function bodies remain local to that function's parser.
    """
    macro_values = _standard_integer_macros()
    macro_values.update(host_preprocessor_macros())
    macro_values.update(numeric_macros or {})
    parser = _Parser(tokens, set(typedef_names) | BUILTIN_TYPE_NAMES,
                     macro_values)
    result = {}
    brace_depth = 0
    n = len(tokens)
    i = 0
    while i < n:
        t = tokens[i]
        if t.kind == "pp":
            i += 1
            continue
        if t.kind == "punct":
            if t.text == "{":
                brace_depth += 1
            elif t.text == "}":
                brace_depth = max(0, brace_depth - 1)
        if t.kind == "kw" and t.text == "enum" and brace_depth == 0:
            j = i + 1
            paren = brack = 0
            body = None
            while j < n:
                tt = tokens[j]
                if tt.kind == "pp":
                    j += 1
                    continue
                if tt.kind == "punct":
                    if tt.text == "(":
                        paren += 1
                    elif tt.text == ")":
                        paren -= 1
                    elif tt.text == "[":
                        brack += 1
                    elif tt.text == "]":
                        brack -= 1
                    elif tt.text == "{" and paren == 0 and brack == 0:
                        body = j
                        break
                    elif tt.text == ";" and paren == 0 and brack == 0:
                        break
                j += 1
            if body is not None:
                parser.i = body
                try:
                    parser._skip_enum_body()
                except ParseError:
                    i = body + 1
                    continue
                result.update(parser.const_scopes[0])
                i = parser.i
                continue
        i += 1
    return result


def parse_file(path, text, extra_typedefs=frozenset(),
               extra_macros=None, extra_defines=None,
               extra_enum_constants=None):
    """Parse a C source file. Returns list of FuncDef.

    extra_typedefs: typedef names collected from other files in the same
    scan, used for declaration/cast disambiguation.
    extra_macros: numeric object-like macro values collected across the
    scan, used for array bounds and preprocessor conditionals.
    extra_defines: compile-definition names (optionally NAME=value) used
    for preprocessor conditionals.
    extra_enum_constants: enum constant values collected from other files
    in the same scan, used for constant expressions and array bounds.
    """
    raw_tokens = tokenize(text)
    typedef_names = (collect_typedefs(raw_tokens) | BUILTIN_TYPE_NAMES
                     | set(extra_typedefs))
    local_defs = collect_macro_definitions(raw_tokens)
    string_macros = {name: value for name, value in local_defs.items()
                     if value.strip().startswith('"')}
    local_numeric = resolve_numeric_macros(local_defs, seed=extra_macros)
    numeric_macros = _standard_integer_macros()
    numeric_macros.update(host_preprocessor_macros())
    numeric_macros.update(extra_macros or {})
    for name in local_defs:
        numeric_macros.pop(name, None)
    numeric_macros.update(local_numeric)
    pp_env = preprocessor_defines(numeric_macros=numeric_macros,
                                  external_defines=extra_defines)
    preprocessor = _Parser(raw_tokens, typedef_names, numeric_macros,
                           string_macros)
    tokens, unknown_pp_lines = preprocessor._pp_filter(raw_tokens, pp_env)
    file_enum_constants = collect_enum_constants(tokens, typedef_names,
                                                 numeric_macros)
    enum_constants = {**(extra_enum_constants or {}),
                      **file_enum_constants}
    n = len(tokens)

    # Pass 1: find top-level items and function bodies.
    funcs = []
    i = 0
    item_start = 0
    paren = brace = brack = 0
    while i < n:
        t = tokens[i]
        if t.kind == "pp":
            i += 1
            continue
        if t.kind == "punct":
            if t.text == "(":
                paren += 1
            elif t.text == ")":
                paren -= 1
            elif t.text == "[":
                brack += 1
            elif t.text == "]":
                brack -= 1
            elif t.text == "{":
                if paren == 0 and brack == 0 and brace == 0:
                    header = tokens[item_start:i]
                    res = _header_is_function(header)
                    if res is not None:
                        name, lp = res
                        # lp is index into header; param list starts at
                        # the '(' -- find its position in tokens
                        # header = tokens[item_start:i]; lp is index in
                        # header of '('
                        lparen_tok_idx = item_start + lp
                        try:
                            # build parser over whole token stream
                            parser = _Parser(tokens, typedef_names,
                                             numeric_macros, string_macros)
                            parser.const_scopes[0].update(enum_constants)
                            parser.constants.update(enum_constants)
                            parser.i = lparen_tok_idx
                            parser.func_name = name
                            # declare params in function scope
                            parser.scope = [{}]
                            params, after = parser.parse_params(
                                lparen_tok_idx)
                            param_names = [
                                (parser.declare(p[0]), p[1], p[2])
                                for p in params
                            ]
                            # skip attributes between ')' and '{'
                            parser.i = after
                            parser.skip_attribute()
                            parser.skip_pp()
                            # expect '{'
                            if not parser.at_punct("{"):
                                # K&R or weird: skip body, mark unsupported
                                depth = 0
                                j = i
                                while j < n:
                                    if tokens[j].kind == "punct":
                                        if tokens[j].text == "{":
                                            depth += 1
                                        elif tokens[j].text == "}":
                                            depth -= 1
                                            if depth == 0:
                                                break
                                    j += 1
                                funcs.append(FuncDef(
                                    name, param_names, ("opaque", tokens[i].line),
                                    tokens[i].line, ["unsupported function header"],
                                    path))
                                i = j + 1
                                item_start = i
                                continue
                            body = parser.parse_block()
                            body_end_line = tokens[parser.i - 1].line \
                                if parser.i > 0 else tokens[i].line
                            funcs.append(FuncDef(
                                name, param_names, body, tokens[i].line,
                                list(parser.partial) + [
                                    "unresolved preprocessor conditional"
                                ] * sum(
                                    1 for line in unknown_pp_lines
                                    if tokens[i].line <= line <=
                                    body_end_line), path))
                            i = parser.i
                            item_start = i
                            paren = brace = brack = 0
                            continue
                        except ParseError as e:
                            # skip balanced body
                            depth = 0
                            j = i
                            while j < n:
                                if tokens[j].kind == "punct":
                                    if tokens[j].text == "{":
                                        depth += 1
                                    elif tokens[j].text == "}":
                                        depth -= 1
                                        if depth == 0:
                                            break
                                j += 1
                            funcs.append(FuncDef(
                                name, [], ("opaque", tokens[i].line),
                                tokens[i].line,
                                [f"parse error: {e}"], path))
                            i = j + 1
                            item_start = i
                            paren = brace = brack = 0
                            continue
                    else:
                        # struct/enum/union def or initializer: skip braces
                        brace += 1  # treat as depth so we don't retrigger
                else:
                    brace += 1
            elif t.text == "}":
                brace -= 1
            elif t.text == ";" and paren == 0 and brace == 0 and brack == 0:
                item_start = i + 1
        i += 1
    return funcs
