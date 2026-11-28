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
    sizeof, prefix/postfix ++/--

Not supported (function marked partial or unsupported):
  * preprocessor lines inside function bodies (marked partial)
  * statement expressions, compound literals, _Generic, computed goto,
    inline asm, VLA dimensions that are not integer constants
  * function-pointer declarators (parsed best-effort, marked partial)
  * K&R-style definitions, nested functions, C++ constructs
"""

from __future__ import annotations

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
# _Alignof/_Alignas are excluded: they appear inside declarations or as
# expressions, never as the first token of one.
DECL_SPEC_KWS = SPEC_KWS - {"_Alignof", "_Alignas"}

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
    def __init__(self, tokens, typedef_names):
        self.toks = tokens
        self.i = 0
        self.partial = []
        self.typedef_names = typedef_names
        self.func_name = ""
        self.scope = [{}]          # name -> unique name
        self.uid = 0
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
        if t.kind == "id" and (t.text in self.typedef_names
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
        """
        seen_type = False
        while True:
            t = self.peek()
            if t.kind == "pp":
                self.skip_pp()
                continue
            if t.kind == "kw" and t.text in SPEC_KWS:
                self.next()
                if t.text in ("struct", "union", "enum"):
                    if self.peek().kind == "id":
                        self.next()
                    if self.at_punct("{"):
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

    def lookup(self, name):
        for scope in reversed(self.scope):
            if name in scope:
                return scope[name]
        # undeclared (shouldn't happen in valid C); make stable pseudo-name
        return f"?{name}"

    def parse_declarator(self, in_for=False):
        """Parse one declarator. Returns VarDecl."""
        line = self.peek().line
        ptr = 0
        while self.at_punct("*"):
            ptr += 1
            self.next()
            while self.peek().text in QUALIFIER_KWS:
                self.next()
        is_funcptr = False
        name = None
        if self.at_punct("("):
            # function pointer declarator: (*name)(params) or nested
            is_funcptr = True
            self.next()  # consume '('
            while self.at_punct("*") or self.peek().text in QUALIFIER_KWS:
                self.next()
            if self.peek().kind == "id":
                name = self.next().text
            # consume remainder of the declarator group '(...name...)'
            depth = 1
            while depth:
                t = self.next()
                if t.kind == "eof":
                    raise ParseError("unbalanced funcptr declarator")
                if t.kind == "punct":
                    if t.text == "(":
                        depth += 1
                    elif t.text == ")":
                        depth -= 1
            # parameter list(s) and array dims of the declarator
            while self.at_punct("("):
                self.skip_balanced("(", ")")
            while self.at_punct("["):
                self.skip_balanced("[", "]")
            if name is None:
                raise ParseError("funcptr declarator without name")
        else:
            if self.peek().kind != "id":
                raise ParseError(f"expected declarator name at line {line}")
            name = self.next().text
            # parenthesized declarator not handled -> bail
            if self.at_punct("("):
                raise ParseError("parenthesized declarator")
        dims = []
        vla = False
        while self.at_punct("["):
            self.next()
            if self.at_punct("]"):
                dims.append(None)
                self.next()
                continue
            e = self.parse_expr()
            dims.append(e)
            if e[0] != "num":
                vla = True
            self.expect_punct("]")
        init = None
        complex_init = False
        if self.at_punct("="):
            self.next()
            if self.at_punct("{"):
                self.skip_balanced("{", "}")
                complex_init = True
            else:
                init = self.parse_expr()
        uniq = self.declare(name)
        if vla:
            self.partial.append("variably-modified array type")
        if is_funcptr:
            self.partial.append("function-pointer declarator")
        return VarDecl(uniq, ptr, dims, "complex" if complex_init else init,
                       is_funcptr, line, vla)

    def parse_decl(self, for_mode=False):
        """Parse a declaration; returns list of VarDecl.
        In for_mode the declaration ends at ')' instead of ';'."""
        self.skip_specifiers()
        decls = []
        while True:
            d = self.parse_declarator()
            decls.append(d)
            if self.at_punct(","):
                self.next()
                continue
            break
        return decls

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
                if not (t.text in self.typedef_names
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
            if t.text in ("!", "~", "-", "+", "*", "&", "++", "--"):
                self.next()
                e = self._parse_unary()
                if t.text == "&":
                    return ("addr", e, t.line)
                return ("unary", t.text, e, t.line)
            if t.text == "(":
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
            return ("num", "alignof", t.line)
        if t.kind == "kw" and t.text in ("_Generic",):
            self.partial.append("_Generic expression")
            # skip balanced parens best-effort
            if self.at_punct("("):
                self.skip_balanced("(", ")")
            return ("unknown", t.line)
        if t.kind == "id" and t.text in ("asm", "__asm", "__asm__"):
            self.partial.append("inline assembly")
            if self.at_punct("(") or self.at_punct("volatile"):
                if self.at_punct("volatile"):
                    self.next()
                if self.at_punct("("):
                    self.skip_balanced("(", ")")
                elif self.peek().kind == "str":
                    self.next()
            return ("unknown", t.line)
        return self._postfix_loop(self._parse_primary())

    def _postfix_loop(self, e):
        while True:
            t = self.peek()
            if t.kind == "punct":
                if t.text == "(":
                    self.next()
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
        if t.kind == "str":
            # adjacent string literals concatenate in C
            text = t.text
            while self.peek().kind == "str":
                text += self.next().text
            return ("str", text, t.line)
        if t.kind == "chr":
            return ("chr", t.text, t.line)
        if t.kind == "id":
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
                name = self.next()
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
            self.skip_specifiers()
            decls = self.parse_decl()
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


def collect_typedefs(tokens):
    """First pass: gather typedef names from top-level items."""
    names = set()
    n = len(tokens)
    i = 0
    depth = 0
    while i < n:
        t = tokens[i]
        if t.kind == "pp":
            i += 1
            continue
        if t.kind == "punct":
            if t.text in "([{":
                depth += 1
            elif t.text in ")]}":
                depth -= 1
        if (t.kind == "kw" and t.text == "typedef" and depth == 0):
            # scan to ';' at depth 0 relative
            j = i + 1
            d = 0
            candidates = []
            last_was_struct_kw = False
            while j < n:
                tt = tokens[j]
                if tt.kind == "eof":
                    break
                if tt.kind == "punct":
                    if tt.text in "([{":
                        d += 1
                    elif tt.text in ")]}":
                        d -= 1
                    elif tt.text == ";" and d == 0:
                        break
                    elif tt.text == "," and d == 0:
                        candidates.append(None)  # separator marker
                elif tt.kind == "id" and d == 0:
                    candidates.append(tt.text)
                elif tt.kind == "kw" and tt.text in ("struct", "union",
                                                     "enum"):
                    last_was_struct_kw = True
                j += 1
            # candidate names: ids at depth0; drop the one right after
            # struct/union/enum keyword (the tag). Approximate: collect
            # ids, remove first id following those keywords.
            ids = [c for c in candidates if c]
            # remove tag names: an id immediately preceded in token stream
            # by struct/union/enum kw
            real = []
            prev_kw = None
            for k in range(i + 1, j):
                tt = tokens[k]
                if tt.kind == "kw" and tt.text in ("struct", "union",
                                                   "enum"):
                    prev_kw = tt.text
                    continue
                if tt.kind == "id" and tt.text not in SPEC_KWS:
                    if prev_kw in ("struct", "union", "enum"):
                        prev_kw = None
                        continue
                    real.append(tt.text)
                prev_kw = None if tt.kind != "kw" else prev_kw
            # 'real' may include declarator names AND type names used
            # without keyword (e.g. 'typedef foo bar;' -> 'foo' is type,
            # 'bar' is name). Heuristic: the LAST id before ',' or ';' is
            # the declared name; ids before commas in multi-decl are also
            # names. Take ids that are preceded (skipping '*') by start,
            # ',', or '}'. Simpler: take all ids that are NOT known type
            # keywords and appear after the last type-specifier-ish id...
            # Pragmatic: last id of each comma-separated group.
            groups = [[]]
            for tt in tokens[i + 1:j]:
                if tt.kind == "punct" and tt.text == ",":
                    groups.append([])
                elif tt.kind == "id":
                    groups[-1].append(tt.text)
            for g in groups:
                if g:
                    names.add(g[-1])
            i = j
        i += 1
    return names


def parse_file(path, text, extra_typedefs=frozenset()):
    """Parse a C source file. Returns list of FuncDef.

    extra_typedefs: typedef names collected from other files in the same
    scan, used for declaration/cast disambiguation.
    """
    tokens = tokenize(text)
    typedef_names = (collect_typedefs(tokens) | BUILTIN_TYPE_NAMES
                     | set(extra_typedefs))
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
                            parser = _Parser(tokens, typedef_names)
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
                            funcs.append(FuncDef(
                                name, param_names, body, tokens[i].line,
                                list(parser.partial), path))
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
