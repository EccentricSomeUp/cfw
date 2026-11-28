"""Forward flow-sensitive data-flow analysis and bug checks.

Abstract domain (per function):
  * vars: unique-name -> Value
      Value is one of:
        ('ptr', frozenset(obj_ids), may_null, may_unknown)
        ('int', (lo, hi))            lo/hi ints or None = unbounded
        ('array', (dim0_interval,))  fixed-size array variable
        ('addr', name_or_None)       result of '&' (only inside call args)
        ('unknown',) ('void',)
  * objs: obj_id -> [status_set, maybe, escaped, returned, size, site]
      status_set subset of {'live','freed'}; 'maybe' = object need not
      exist on every incoming path.
  * alloc sites are keyed by source position so loops stay bounded;
    realloc results get a deterministic hashed id derived from the old
    object so they also stay bounded.
"""

from __future__ import annotations

import re
import zlib
from dataclasses import dataclass, field

LIVE = "live"
FREED = "freed"

MAX_OBJS = 96
WIDEN_LIMIT = 1 << 20
SIZE_MAX = (1 << 64) - 1

PTR_UNKNOWN = ("ptr", frozenset(), True, True)
INT_UNKNOWN = ("int", (None, None))
UNKNOWN = ("unknown",)
VOID = ("void",)


# ---------------------------------------------------------------- values

def ptr_val(objs=(), null=False, unk=False):
    return ("ptr", frozenset(objs), bool(null), bool(unk))


def int_val(lo=None, hi=None):
    return ("int", (lo, hi))


def interval_of(v):
    if v[0] == "int":
        return v[1]
    return (None, None)


def join_interval(a, b):
    # None means unbounded; the join (hull) of bounded and unbounded is
    # unbounded.
    (lo1, hi1), (lo2, hi2) = a, b
    lo = None if lo1 is None or lo2 is None else min(lo1, lo2)
    hi = None if hi1 is None or hi2 is None else max(hi1, hi2)
    # widening: unbounded-ish results become fully unbounded
    if hi is not None and hi > WIDEN_LIMIT:
        hi = None
    if lo is not None and lo < -WIDEN_LIMIT:
        lo = None
    return (lo, hi)


def join_value(a, b):
    if a[0] == "unknown" or b[0] == "unknown":
        # unknown is top; but keep 'void'
        if a[0] == "void" and b[0] == "void":
            return VOID
        if a[0] == "unknown" and b[0] == "unknown":
            return UNKNOWN
        return UNKNOWN
    if a[0] != b[0]:
        return UNKNOWN
    if a[0] == "ptr":
        return ("ptr",
                a[1] | b[1],
                a[2] or b[2],
                a[3] or b[3])
    if a[0] == "int":
        return int_val(*join_interval(a[1], b[1]))
    if a[0] == "array":
        if a[1] == b[1]:
            return a
        return UNKNOWN
    if a[0] == "addr":
        if a[1] == b[1]:
            return a
        return ("addr", None)
    return a


# ---------------------------------------------------------------- state

class State:
    __slots__ = ("vars", "objs", "outmap")

    def __init__(self, vars=None, objs=None, outmap=None):
        self.vars = vars if vars is not None else {}
        self.objs = objs if objs is not None else {}
        # status_var -> frozenset(ptr vars) written by the out-param
        # allocator whose result was assigned to status_var
        self.outmap = outmap if outmap is not None else {}

    def copy(self):
        return State(dict(self.vars),
                     {k: list(v) for k, v in self.objs.items()},
                     dict(self.outmap))

    def join(self, other):
        vars_ = dict(self.vars)
        for name, v in other.vars.items():
            if name in vars_:
                vars_[name] = join_value(vars_[name], v)
            else:
                vars_[name] = v
        for name in self.vars:
            if name not in other.vars:
                # variable not present on other path: keep, but for ptr
                # values the 'maybe' notion is per-object; keep value.
                pass
        objs = {}
        for oid, o in self.objs.items():
            if oid in other.objs:
                p = other.objs[oid]
                objs[oid] = [o[0] | p[0], o[1] or p[1],
                             o[2] or p[2], o[3] or p[3],
                             join_interval(o[4], p[4]) if o[4] and p[4]
                             else (o[4] or p[4]), o[5], o[6],
                             o[7] if len(o) > 7 and o[7] else
                             (p[7] if len(p) > 7 else None)]
            else:
                oo = list(o)
                oo[1] = True  # absent on other path -> maybe
                objs[oid] = oo
        for oid, p in other.objs.items():
            if oid not in self.objs:
                pp = list(p)
                pp[1] = True
                objs[oid] = pp
        outmap = {}
        for k, s1 in self.outmap.items():
            if k in other.outmap:
                inter = s1 & other.outmap[k]
                if inter:
                    outmap[k] = inter
        return State(vars_, objs, outmap)


# obj record layout: [status:set, maybe:bool, escaped:bool, returned:bool,
#                     size:(lo,hi)|None, alloc_line:int, site:str,
#                     realloc_parent:oid|None]


def new_obj(size=None, line=0, site="", parent=None):
    return [{LIVE}, False, False, False, size, line, site, parent]


# ---------------------------------------------------------------- findings

@dataclass(frozen=True)
class Finding:
    rule: str
    severity: str
    confidence: str
    file: str
    line: int
    col: int
    message: str
    trace: str = ""


# ---------------------------------------------------------------- analyzer

class FunctionAnalyzer:
    def __init__(self, func, findings, config):
        self.func = func
        self.findings = findings
        self.config = config
        self.alloc_fns = set(config.get("alloc", [])) | {
            "malloc", "valloc"}
        self.calloc_fns = set(config.get("calloc", [])) | {"calloc"}
        self.realloc_fns = set(config.get("realloc", [])) | {"realloc"}
        self.free_fns = set(config.get("free", [])) | {"free"}
        self.builtin_free = {"free"}
        self.out_param_fns = dict(config.get("out_param", {}))
        self.borrow_fns = set(config.get("borrow", [])) | {
            "memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp",
            "strncmp", "strcasecmp", "strncasecmp", "strchr", "strrchr",
            "strstr", "strspn", "strcspn", "strpbrk", "strtok", "strcpy",
            "strncpy", "strcat", "strncat", "sprintf", "vsprintf",
            "snprintf", "vsnprintf", "sscanf", "fscanf", "scanf",
            "va_start", "va_end", "va_copy",
            "__builtin_va_start", "__builtin_va_end", "__builtin_va_copy",
            "cfw_arena_reset", "cfw_arena_alloc", "cfw_arena_alloc_zeroed",
        }
        self.alloc_out_fns = {"posix_memalign": 0, "aligned_alloc": None}
        self.alloc_handlers = {
            "cfw_arena_create": self._alloc_simple,
        }
        self.free_handlers = {
            "cfw_arena_destroy": self._release_arg0,
        }
        self.out_handlers = {
            "cfw_arena_alloc": 2, "cfw_arena_alloc_zeroed": 2,
            "posix_memalign": 0,
        }
        self.alloc_size_fns = {"malloc", "calloc", "realloc", "strdup",
                               "strndup", "valloc", "aligned_alloc"}
        self._site_ids = {}
        self._reported = set()
        self._last_out_writes = set()
        self._report_enabled = True

    # ---------------------------------------------------------- helpers
    def site_id(self, expr):
        if expr[0] == "call":
            key = f"{expr[3]}:{id(expr)}"
            # use structural key: callee name + line is not unique enough;
            # expr tuples are shared per AST node so id() is stable within
            # a function analysis
            return key
        return f"{expr[-1] if expr else 0}"

    def report(self, rule, confidence, line, col, message, trace=""):
        if not self._report_enabled:
            return
        f = Finding(rule, "warning" if confidence == "possible"
                    else "error", confidence, self.func.file, line, col,
                    message, trace)
        self.findings.add(f)

    def obj_desc(self, oid):
        o = self.objs_get(oid)
        if o is None:
            return f"object@{oid}"
        return f"object allocated at line {o[5]}"

    def objs_get(self, oid):
        return self._cur_objs.get(oid)

    _cur_objs = None

    # ---------------------------------------------------------- eval
    def eval(self, st, expr):
        """Evaluate expr in state st (mutated for calls/assignments)."""
        tag = expr[0]
        if tag == "num":
            return self.eval_num(expr[1])
        if tag == "str":
            return ptr_val((), null=False, unk=True)
        if tag == "chr":
            return self.eval_num(expr[1])
        if tag == "var":
            n = expr[1]
            if n == "?NULL" or n == "?nullptr":
                return ptr_val((), null=True, unk=False)
            return st.vars.get(n, UNKNOWN)
        if tag == "unknown":
            return UNKNOWN
        if tag == "addr":
            inner = expr[1]
            if inner[0] == "var":
                return ("addr", inner[1])
            return ("addr", None)
        if tag == "unary":
            op = expr[1]
            e = expr[2]
            if op == "&":
                return self.eval_addr(st, e)
            if op == "*":
                v = self.eval(st, e)
                self.check_ptr_use(st, v, expr[3], write=False)
                return UNKNOWN
            if op == "!":
                v = self.eval(st, e)
                return int_val(0, 1)
            if op in ("-", "+", "~"):
                v = self.eval(st, e)
                if v[0] == "int":
                    return self.int_unary(op, v[1])
                return UNKNOWN
            if op in ("++", "--"):
                return self.eval_incdec(st, e, expr[3], prefix=True,
                                        delta=1 if op == "++" else -1)
            return UNKNOWN
        if tag == "postfix":
            return self.eval_incdec(st, expr[2], expr[3], prefix=False,
                                    delta=1 if expr[1] == "++" else -1)
        if tag == "binary":
            return self.eval_binary(st, expr)
        if tag == "assign":
            return self.eval_assign(st, expr)
        if tag == "cond":
            cv = self.eval(st, expr[1])
            # evaluate both arms conservatively in joined state
            tv = self.eval(st, expr[2])
            fv = self.eval(st, expr[3])
            return join_value(tv, fv)
        if tag == "sizeof":
            # sizeof does not evaluate its operand: no dereference, no
            # side effects.
            return int_val(*self._sizeof_interval(st, expr[1]))
        if tag == "va_arg":
            self.eval(st, expr[1])
            type_text = expr[2]
            if "*" in type_text:
                return ptr_val((), null=True, unk=True)
            return UNKNOWN
        if tag == "cast":
            v = self.eval(st, expr[2])
            ts = expr[1]
            # pointer/address casts preserve the underlying object or
            # target variable -- do not lose tracking through (T*)p or
            # (void **)&out
            if v[0] in ("ptr", "addr"):
                return v
            if "*" in ts:
                return ptr_val((), null=False, unk=True)
            if v[0] == "int":
                return self.eval_trunc(v[1], ts, expr[3])
            return UNKNOWN if v[0] != "void" else VOID
        if tag == "member":
            obj = self.eval(st, expr[1])
            if expr[3]:  # ->
                self.check_ptr_use(st, obj, expr[4], write=False)
            return UNKNOWN
        if tag == "index":
            base = self.eval(st, expr[1])
            idx = self.eval(st, expr[2])
            self.check_index(st, base, idx, expr[3])
            return UNKNOWN
        if tag == "call":
            return self.eval_call(st, expr)
        if tag == "comma":
            v = UNKNOWN
            for e in expr[1]:
                v = self.eval(st, e)
            return v
        if tag == "compound":
            return UNKNOWN
        return UNKNOWN

    def _sizeof_interval(self, st, e):
        """Byte-size interval for a sizeof operand expression."""
        if e is not None and e[0] == "var":
            v = st.vars.get(e[1])
            if v is not None and v[0] == "array":
                sv = v[1][1] if len(v[1]) > 1 else None
                if sv is not None:
                    return sv
            elif v is not None and v[0] == "ptr":
                return (8, 8)
        return (1, None)

    def eval_num(self, text):
        t = text.rstrip("uUlLfF")
        try:
            if t.lower().startswith("0x"):
                v = int(t, 16)
            elif len(t) > 1 and t[0] == "0" and t[1].isdigit():
                try:
                    v = int(t, 8)
                except ValueError:
                    v = int(t, 10)
            elif t.replace(".", "", 1).replace("e", "").replace("E", "") \
                    .replace("+", "").replace("-", "").isdigit() \
                    and ("." in t or "e" in t.lower()):
                return UNKNOWN  # float literal as int: unknown
            else:
                v = int(t, 10)
        except ValueError:
            return UNKNOWN
        if v > (1 << 63):
            return int_val(None, None)
        return int_val(v, v)

    def int_unary(self, op, iv):
        lo, hi = iv
        if op == "-":
            nlo = None if hi is None else (-hi if hi >= -(1 << 62) else None)
            nhi = None if lo is None else (-lo if lo <= (1 << 62) else None)
            return int_val(nlo, nhi)
        if op == "~":
            # ~x = -x-1
            r = self.int_unary("-", iv)
            return self.int_add(r[1] if isinstance(r, tuple) else r,
                                int_val(-1, -1)[1], None) if False else \
                self._int_neg1(r)
        if op == "+":
            return int_val(lo, hi)
        return INT_UNKNOWN

    def _int_neg1(self, v):
        iv = v[1] if v[0] == "int" else (None, None)
        return self.int_add(iv, (-1, -1))

    def int_add(self, a, b):
        (lo1, hi1), (lo2, hi2) = a, b
        lo = None if lo1 is None or lo2 is None else lo1 + lo2
        hi = None if hi1 is None or hi2 is None else hi1 + hi2
        if lo is not None and lo < -(1 << 62):
            lo = None
        if hi is not None and hi > (1 << 62):
            hi = None
        return int_val(lo, hi)

    def int_mul(self, a, b):
        (lo1, hi1), (lo2, hi2) = a, b
        if lo1 is None or hi1 is None or lo2 is None or hi2 is None:
            # if either side is definitely 0 -> 0
            if (lo1 == hi1 == 0) or (lo2 == hi2 == 0):
                return int_val(0, 0)
            return int_val(None, None)
        prods = [lo1 * lo2, lo1 * hi2, hi1 * lo2, hi1 * hi2]
        lo, hi = min(prods), max(prods)
        if lo < -(1 << 62) or hi > (1 << 62):
            return int_val(None, None)
        return int_val(lo, hi)

    def eval_binary(self, st, expr):
        op = expr[1]
        l = expr[2]
        r = expr[3]
        if op == "&&":
            self.eval(st, l)
            s_true = self.refine(st.copy(), l, True)
            self.eval(s_true, r)
            s_false = self.refine(st.copy(), l, False)
            st.vars = s_true.vars
            st.objs = s_true.objs
            # merge: state after && is join of (l false) and (l true, r done)
            merged = s_false.join(st)
            st.vars, st.objs, st.outmap = (merged.vars, merged.objs,
                                           merged.outmap)
            return int_val(0, 1)
        if op == "||":
            self.eval(st, l)
            s_false = self.refine(st.copy(), l, False)
            self.eval(s_false, r)
            s_true = self.refine(st.copy(), l, True)
            st.vars, st.objs, st.outmap = s_false.vars, s_false.objs, \
                s_false.outmap
            merged = s_true.join(st)
            st.vars, st.objs, st.outmap = (merged.vars, merged.objs,
                                           merged.outmap)
            return int_val(0, 1)
        lv = self.eval(st, l)
        rv = self.eval(st, r)
        if op in ("==", "!="):
            return int_val(0, 1)
        if op in ("<", "<=", ">", ">=", "&&", "||"):
            return int_val(0, 1)
        if op in ("+", "-"):
            if lv[0] == "int" and rv[0] == "int":
                if op == "+":
                    return self.int_add(lv[1], rv[1])
                # subtraction
                nr = (None if rv[1][1] is None else -rv[1][1],
                      None if rv[1][0] is None else -rv[1][0])
                return self.int_add(lv[1], nr)
            return UNKNOWN
        if op in ("*", "/", "%"):
            if lv[0] == "int" and rv[0] == "int":
                if op == "*":
                    return self.int_mul(lv[1], rv[1])
                return INT_UNKNOWN
            return UNKNOWN
        if op in ("&", "|", "^", "<<", ">>"):
            return INT_UNKNOWN if lv[0] == "int" and rv[0] == "int" \
                else UNKNOWN
        return UNKNOWN

    def eval_addr(self, st, e):
        if e[0] == "var":
            return ("addr", e[1])
        # &*p == p ; &a[i] etc -> unknown address
        if e[0] == "unary" and e[1] == "*":
            return self.eval(st, e[2])
        return ("addr", None)

    def eval_incdec(self, st, target, line, prefix, delta):
        # target: var / unary-* / member / index
        if target[0] == "var":
            old = st.vars.get(target[1], UNKNOWN)
        else:
            old = self.eval(st, target)
        # compute the new value
        if old[0] == "int":
            new = self.int_add(old[1], (delta, delta))
        elif old[0] == "ptr":
            # pointer arithmetic stays inside the same object(s)
            new = ptr_val(old[1], old[2], True)
        else:
            new = old
        self.assign_to(st, target, new, line, is_incdec=True)
        return old

    def assign_to(self, st, lhs, value, line, is_incdec=False):
        """Assign value to lvalue expr; handles deref/member/index writes."""
        tag = lhs[0]
        if tag == "var":
            name = lhs[1]
            cur = st.vars.get(name)
            # keep array-ness: assigning to array var is invalid; ignore
            if cur is not None and cur[0] == "array":
                return
            st.vars[name] = value
            st.outmap.pop(name, None)
            return
        # write through pointer/member/index: check validity, escape rhs
        base_val = None
        if tag == "unary" and lhs[1] == "*":
            base_val = self.eval(st, lhs[2])
            self.check_ptr_use(st, base_val, line, write=True)
        elif tag == "member":
            base_val = self.eval(st, lhs[1])
            if lhs[3]:
                self.check_ptr_use(st, base_val, line, write=True)
        elif tag == "index":
            base_val = self.eval(st, lhs[1])
            iv = self.eval(st, lhs[2])
            self.check_index(st, base_val, iv, line, write=True)
        if value[0] == "ptr":
            self.escape_objs(st, value[1])

    def eval_assign(self, st, expr):
        op = expr[1]
        lhs = expr[2]
        rhs = expr[3]
        if op == "=":
            v = self.eval(st, rhs)
            self.assign_to(st, lhs, v, expr[4])
            if lhs[0] == "var" and rhs[0] == "call" \
                    and self._last_out_writes:
                cname = self.callee_name(rhs[1])
                if cname in self.out_handlers:
                    st.outmap[lhs[1]] = frozenset(self._last_out_writes)
            return v
        # compound assignment: eval both, produce approx value, assign
        lv = self.eval(st, lhs)
        rv = self.eval(st, rhs)
        res = UNKNOWN
        if lv[0] == "int" and rv[0] == "int":
            res = INT_UNKNOWN
        if lv[0] == "ptr" and op in ("+=", "-="):
            res = ptr_val(lv[1], lv[2], True)
        self.assign_to(st, lhs, res if res != UNKNOWN else UNKNOWN,
                       expr[4])
        return res

    # ---------------------------------------------------------- checks
    def check_ptr_use(self, st, v, line, write, col=0):
        """Check a pointer value used for deref/read/write."""
        if v[0] != "ptr":
            return
        objs, may_null, may_unk = v[1], v[2], v[3]
        self._cur_objs = st.objs
        for oid in objs:
            o = st.objs.get(oid)
            if o is None:
                continue
            status = o[0]
            if status == {FREED}:
                self.report("USE_AFTER_FREE", "definite", line, col,
                            f"use of {self.obj_desc(oid)} after it was freed",
                            self.trace_for(oid))
            elif FREED in status:
                self.report("USE_AFTER_FREE", "possible", line, col,
                            f"possible use of {self.obj_desc(oid)} "
                            "(freed on some paths)", self.trace_for(oid))
        if may_null:
            if objs or may_unk:
                self.report("NULL_DEREF", "possible", line, col,
                            "pointer may be null at dereference")
            else:
                self.report("NULL_DEREF", "definite", line, col,
                            "pointer is null at dereference")

    def check_index(self, st, base, idx, line, write=False):
        if base[0] == "array":
            dim = base[1][0] if base[1] else None
            self.check_bounds(st, idx, dim, line, write)
            return
        if base[0] == "ptr":
            self.check_ptr_use(st, base, line, write=write)
            if idx[0] == "int":
                for oid in base[1]:
                    o = st.objs.get(oid)
                    if o and o[4]:
                        self.check_bounds(st, idx, o[4], line, write,
                                          obj=oid)

    def check_bounds(self, st, idx, size_iv, line, write, obj=None):
        if idx[0] != "int" or size_iv is None:
            return
        (ilo, ihi) = idx[1]
        (slo, shi) = size_iv
        if slo is None or shi is None:
            return
        size = shi  # use upper bound as capacity
        if ilo is not None and ilo >= size:
            self.report("OUT_OF_BOUNDS", "definite", line, 0,
                        f"index is always >= size {size}")
        elif ihi is not None and ihi >= size and (ilo is None or ilo < size):
            self.report("OUT_OF_BOUNDS", "possible", line, 0,
                        f"index may exceed size {size}")
        elif ihi is None and ilo is not None and ilo < size:
            # index grows without a known bound (e.g. loop induction
            # variable compared against an unknown limit)
            self.report("OUT_OF_BOUNDS", "possible", line, 0,
                        f"index may grow beyond size {size}")
        if ilo is not None and ilo < 0:
            self.report("OUT_OF_BOUNDS", "definite", line, 0,
                        "index may be negative")

    def trace_for(self, oid):
        o = self._cur_objs.get(oid) if self._cur_objs else None
        if o and o[5]:
            return f"allocated at {self.func.file}:{o[5]}"
        return ""

    def escape_objs(self, st, obj_ids):
        for oid in obj_ids:
            o = st.objs.get(oid)
            if o is not None:
                o[2] = True

    def escape_var_ptr(self, st, name):
        v = st.vars.get(name)
        if v is not None and v[0] == "ptr":
            self.escape_objs(st, v[1])
            st.vars[name] = ("ptr", frozenset(), True, True)

    # ---------------------------------------------------------- calls
    def callee_name(self, func_expr):
        if func_expr[0] == "var":
            n = func_expr[1]
            return n[1:] if n.startswith("?") else n
        return None

    def eval_call(self, st, expr):
        func_expr = expr[1]
        args = expr[2]
        line = expr[3]
        name = self.callee_name(func_expr)
        if func_expr[0] != "var":
            # indirect/member call: evaluate for deref checks, treat as
            # unknown function
            self.eval(st, func_expr)
            name = None
        argvals = [self.eval(st, a) for a in args]
        self._last_out_writes = set()

        # CFW_TEST_ASSERT* macros abort the test on failure, so the
        # enclosed condition holds on the fallthrough path.
        if name is not None and name.startswith("CFW_TEST_ASSERT"):
            self._apply_test_assert(st, name, args)
            return VOID

        # modeled functions
        if name in self.free_fns or name in self.builtin_free \
                or name in self.free_handlers:
            self.do_free(st, argvals, line, name)
            return VOID
        if name in self.realloc_fns:
            return self.do_realloc(st, argvals, line)
        if name in self.calloc_fns:
            self.check_alloc_size(st, argvals, line, name, calloc=True)
            return self.do_alloc(st, self._size_of_calloc(argvals), line,
                                 name, site=id(expr))
        if name in self.alloc_fns:
            self.check_alloc_size(st, argvals, line, name)
            size = argvals[0][1] if argvals and argvals[0][0] == "int" \
                else (None, None)
            return self.do_alloc(st, size, line, name, site=id(expr))
        if name == "strdup" or name == "strndup":
            return self.do_alloc(st, None, line, name, site=id(expr))
        if name == "aligned_alloc":
            size = argvals[1][1] if len(argvals) > 1 \
                and argvals[1][0] == "int" else (None, None)
            self.check_alloc_size(st, [argvals[1]] if argvals else [],
                                  line, name)
            return self.do_alloc(st, size, line, name, site=id(expr))
        if name in self.out_handlers:
            idx = self.out_handlers[name]
            self.do_out_alloc(st, argvals, idx, line, name)
            return int_val(0, 1)  # status code
        if name in self.alloc_handlers:
            return self.alloc_handlers[name](st, argvals, line)
        if name in self.borrow_fns:
            self.check_mem_args(st, name, argvals, args, line)
            return UNKNOWN
        # default: unknown call -- escape pointer arguments
        self.escape_call_args(st, args, argvals)
        return UNKNOWN

    def _alloc_simple(self, st, argvals, line):
        size = argvals[0][1] if argvals and argvals[0][0] == "int" \
            else (None, None)
        return self.do_alloc(st, size, line, "cfw_arena_create")

    def _release_arg0(self, st, argvals, line):
        if argvals:
            self.do_free(st, argvals[:1], line, "cfw_arena_destroy")
        return VOID

    def _size_of_calloc(self, argvals):
        if len(argvals) >= 2 and argvals[0][0] == "int" \
                and argvals[1][0] == "int":
            return self.int_mul(argvals[0][1], argvals[1][1])[1] \
                if isinstance(self.int_mul(argvals[0][1], argvals[1][1]),
                              tuple) else None
        return None

    def do_alloc(self, st, size, line, name, site=None):
        oid = f"{name}@{site if site is not None else line}"
        # include column-ish uniqueness via expr handled by caller site
        if oid in st.objs:
            # same site re-executed (loop): fresh object
            st.objs[oid] = new_obj(size, line, name)
        else:
            if len(st.objs) >= MAX_OBJS:
                return ptr_val((), null=True, unk=True)
            st.objs[oid] = new_obj(size, line, name)
        return ptr_val((oid,), null=True, unk=False)

    def do_out_alloc(self, st, argvals, idx, line, name):
        """Allocator writing result through &out (e.g. cfw_arena_alloc,
        posix_memalign). Arena-style objects are marked escaped."""
        oid = f"{name}@out{idx}@{id(argvals)}"
        if oid not in st.objs:
            if len(st.objs) >= MAX_OBJS:
                return
            st.objs[oid] = new_obj(None, line, name)
            st.objs[oid][2] = name.startswith("cfw_arena")
        if idx < len(argvals):
            av = argvals[idx]
            if av[0] == "addr" and av[1] is not None:
                old = st.vars.get(av[1], UNKNOWN)
                objs = set(old[1]) if old[0] == "ptr" else set()
                objs.add(oid)
                st.vars[av[1]] = ptr_val(objs, null=True, unk=False)
                self._last_out_writes.add(av[1])

    def do_realloc(self, st, argvals, line):
        if not argvals:
            return UNKNOWN
        pv = argvals[0]
        old_objs = set()
        if pv[0] == "ptr":
            old_objs = set(pv[1])
            # realloc requires live object
            self._cur_objs = st.objs
            for oid in old_objs:
                o = st.objs.get(oid)
                if o and o[0] == {FREED}:
                    self.report("USE_AFTER_FREE", "definite", line, 0,
                                f"realloc of {self.obj_desc(oid)} "
                                "after it was freed", self.trace_for(oid))
                elif o and FREED in o[0]:
                    self.report("USE_AFTER_FREE", "possible", line, 0,
                                f"realloc of {self.obj_desc(oid)} "
                                "possibly after free", self.trace_for(oid))
            # mark old objects may-freed (success) / live (failure)
            for oid in old_objs:
                o = st.objs.get(oid)
                if o is not None:
                    o[0] = {LIVE, FREED}
        size = argvals[1][1] if len(argvals) > 1 \
            and argvals[1][0] == "int" else (None, None)
        # deterministic new id derived from old object
        if old_objs:
            old = sorted(old_objs)[0]
            slot = zlib.crc32(f"{old}".encode()) % 16
            oid = f"realloc@{line}~{slot}"
        else:
            oid = f"realloc@{line}"
        parent = sorted(old_objs)[0] if len(old_objs) == 1 else None
        if oid not in st.objs:
            if len(st.objs) >= MAX_OBJS:
                return ptr_val((), null=True, unk=True)
            st.objs[oid] = new_obj(size, line, "realloc", parent=parent)
        else:
            st.objs[oid] = new_obj(size, line, "realloc", parent=parent)
        # result: new object, may be NULL on failure
        return ptr_val((oid,), null=True, unk=False)

    def do_free(self, st, argvals, line, name="free"):
        if not argvals:
            return
        self._cur_objs = st.objs
        v = argvals[0]
        if v[0] != "ptr":
            return
        may_null = v[2]
        for oid in v[1]:
            o = st.objs.get(oid)
            if o is None:
                continue
            status = o[0]
            if status == {FREED}:
                conf = "possible" if may_null else "definite"
                self.report("DOUBLE_FREE", conf, line, 0,
                            f"{name} of {self.obj_desc(oid)} which was "
                            "already freed" + (" (pointer may be null)"
                                               if may_null else ""),
                            self.trace_for(oid))
                o[0] = {FREED}
            elif FREED in status:
                self.report("DOUBLE_FREE", "possible", line, 0,
                            f"{name} of {self.obj_desc(oid)} which may "
                            "already be freed on some paths",
                            self.trace_for(oid))
                o[0] = {FREED}
            else:
                o[0] = {FREED}

    def check_alloc_size(self, st, argvals, line, name, calloc=False):
        if not argvals:
            return
        ivs = [a[1] for a in argvals if a[0] == "int"]
        if not ivs:
            return
        if calloc and len(ivs) >= 2:
            prod = self.int_mul(ivs[0], ivs[1])[1]
            rng = prod if isinstance(prod, tuple) else prod
            iv = rng
        else:
            iv = ivs[0]
        if not isinstance(iv, tuple):
            return
        lo, hi = iv
        if hi is not None and hi > SIZE_MAX:
            conf = "definite" if (lo is not None and lo > SIZE_MAX) \
                else "possible"
            self.report("ALLOC_SIZE_OVERFLOW", conf, line, 0,
                        f"allocation size for {name} may exceed SIZE_MAX")

    def check_mem_args(self, st, name, argvals, arg_exprs, line):
        """Bounds checks for modeled memory functions."""
        # (ptr_arg_index, len_arg_index) pairs
        specs = {
            "memcpy": (0, 2, 1, 2), "memmove": (0, 2, 1, 2),
            "memset": (0, 2, None, None),
            "strncpy": (0, 2, 1, 2), "strncat": (0, 2, 1, 2),
            "snprintf": (0, 1, None, None), "vsnprintf": (0, 1, None,
                                                         None),
            "strcpy": (0, None, 1, None), "strcat": (0, None, 1, None),
            "sprintf": (0, None, None, None),
        }
        if name not in specs:
            return
        d0, n0, s0, n1 = specs[name]
        self._check_mem_pair(st, argvals, d0, n0, line, name, write=True)
        if s0 is not None:
            self._check_mem_pair(st, argvals, s0, n1, line, name,
                                 write=False)

    def _check_mem_pair(self, st, argvals, pidx, nidx, line, name, write):
        if pidx >= len(argvals):
            return
        pv = argvals[pidx]
        if pv[0] != "ptr":
            return
        self.check_ptr_use(st, pv, line, write=write)
        niv = None
        if nidx is not None and nidx < len(argvals) \
                and argvals[nidx][0] == "int":
            niv = argvals[nidx][1]
        for oid in pv[1]:
            o = st.objs.get(oid)
            if o and o[4] and niv:
                (nlo, nhi) = niv
                (slo, shi) = o[4]
                if shi is not None and nhi is not None and nhi > shi:
                    conf = "definite" if (nlo is not None and nlo > shi) \
                        else "possible"
                    self.report("OUT_OF_BOUNDS", conf, line, 0,
                                f"{name} length may exceed allocation "
                                f"size {shi}")

    def escape_call_args(self, st, arg_exprs, argvals):
        for av in argvals:
            if av[0] == "ptr":
                self.escape_objs(st, av[1])
            elif av[0] == "addr" and av[1] is not None:
                # callee may free or store through the pointer-to-pointer;
                # it may also overwrite the variable itself
                self.escape_var_ptr(st, av[1])

    def _apply_test_assert(self, st, name, args):
        """Refine state by the condition inside a CFW_TEST_ASSERT* call."""
        cond = None
        want = True
        if name in ("CFW_TEST_ASSERT_EQ", "CFW_TEST_ASSERT_NE") \
                and len(args) >= 2:
            op = "==" if name.endswith("_EQ") else "!="
            cond = ("binary", op, args[0], args[1],
                    args[0][-1] if args[0] else 0)
        elif name == "CFW_TEST_ASSERT" and args:
            cond = args[0]
        elif name in ("CFW_TEST_ASSERT_TRUE", "CFW_TEST_ASSERT_FALSE") \
                and args:
            cond = args[0]
            want = name.endswith("TRUE")
        if cond is None:
            return
        ref = self.refine(st.copy(), cond, want)
        st.vars, st.objs, st.outmap = ref.vars, ref.objs, ref.outmap

    # ---------------------------------------------------------- refine
    def refine(self, st, expr, want_true):
        """Return a new state refined by expr being want_true."""
        tag = expr[0]
        if tag == "binary" and expr[1] in ("&&", "||"):
            if expr[1] == "&&":
                if want_true:
                    s = self.refine(st, expr[2], True)
                    return self.refine(s, expr[3], True)
                sA = self.refine(st, expr[2], False)
                sB = self.refine(self.refine(st, expr[2], True),
                                 expr[3], False)
                return sA.join(sB)
            else:
                if want_true:
                    sA = self.refine(st, expr[2], True)
                    sB = self.refine(self.refine(st, expr[2], False),
                                     expr[3], True)
                    return sA.join(sB)
                s = self.refine(st, expr[2], False)
                return self.refine(s, expr[3], False)
        if tag == "unary" and expr[1] == "!":
            return self.refine(st, expr[2], not want_true)
        if tag == "comma":
            return self.refine(st, expr[1][-1], want_true)
        if tag == "binary" and expr[1] in ("==", "!="):
            eq = (expr[1] == "==")
            want_eq = eq == want_true
            l, r = expr[2], expr[3]
            if self.is_null_expr(l) and r[0] == "var":
                res = self.refine_ptr(st, r[1], want_eq)
            elif self.is_null_expr(r) and l[0] == "var":
                res = self.refine_ptr(st, l[1], want_eq)
            else:
                # integer equality refinement
                res = self.refine_int_cmp(st, expr[1], l, r, want_true)
            self._clear_out_null_on_success(res, expr, want_true)
            return res
        if tag == "binary" and expr[1] in ("<", "<=", ">", ">="):
            return self.refine_int_cmp(st, expr[1], expr[2], expr[3],
                                       want_true)
        if tag == "var":
            v = st.vars.get(expr[1])
            if v is not None and v[0] == "ptr":
                return self.refine_ptr(st, expr[1], not want_true)
            return st
        return st

    def _status_const_kind(self, e):
        """Classify a comparison operand as a success ('ok') status
        constant, an error constant, or unknown."""
        if e[0] == "num":
            try:
                return "ok" if int(e[1], 0) == 0 else "err"
            except (ValueError, TypeError):
                return None
        if e[0] == "var":
            n = e[1].lstrip("?")
            if n in ("CFW_OK", "CFW_SUCCESS", "CFW_NO_ERROR") \
                    or n.endswith("_OK") or n.endswith("_SUCCESS"):
                return "ok"
            if "ERROR" in n or "ERR" in n or "FAIL" in n:
                return "err"
        return None

    def _clear_out_null_on_success(self, st, expr, want_true):
        """Heuristic: ``if (alloc_out(...) != CFW_OK) return;`` implies the
        out-parameters written by that call are non-NULL on the path where
        the status check passes. Clear may_null for pointer variables bound
        to objects produced by modeled out-param allocators."""
        l, r = expr[2], expr[3]
        for call_side, other in ((l, r), (r, l)):
            if call_side[0] != "call":
                continue
            cname = self.callee_name(call_side[1])
            if cname not in self.out_handlers:
                continue
            if self._status_const_kind(other) != "ok":
                continue
            # success = comparison outcome meaning 'status was OK'
            success = want_true if expr[1] == "==" else not want_true
            if not success:
                continue
            for name, v in st.vars.items():
                if v[0] != "ptr" or not v[1]:
                    continue
                hit = False
                for oid in v[1]:
                    o = st.objs.get(oid)
                    if o is not None and o[6] in self.out_handlers:
                        hit = True
                        break
                if hit:
                    st.vars[name] = ("ptr", v[1], False, v[3])
            return
        # indirect form: 'status = alloc_out(...); if (status != OK)'
        for var_side, other in ((l, r), (r, l)):
            if var_side[0] != "var" or var_side[1] not in st.outmap:
                continue
            if self._status_const_kind(other) != "ok":
                continue
            success = want_true if expr[1] == "==" else not want_true
            if not success:
                continue
            for vn in st.outmap[var_side[1]]:
                v = st.vars.get(vn)
                if v is not None and v[0] == "ptr":
                    st.vars[vn] = ("ptr", v[1], False, v[3])
            return

    def is_null_expr(self, e):
        if e[0] == "var" and e[1].lstrip("?") in ("NULL", "nullptr"):
            return True
        if e[0] == "num":
            try:
                return int(e[1], 0) == 0
            except (ValueError, TypeError):
                return False
        if e[0] == "chr":
            return e[1] in ("'\\0'", "'0'")
        return False

    def refine_ptr(self, st, name, is_null):
        st = st.copy()
        v = st.vars.get(name)
        if v is None or v[0] != "ptr":
            return st
        if is_null:
            # p is definitely NULL here: it binds no objects, and the
            # objects it could have pointed to do not exist on this path
            # (e.g. malloc returned NULL), so drop 'live' from their
            # status to avoid phantom leaks.
            for oid in v[1]:
                o = st.objs.get(oid)
                if o is not None:
                    o[0] = set(o[0])
                    o[0].discard(LIVE)
                    # realloc correlation: a NULL realloc result means the
                    # reallocation failed, so the parent object is still
                    # live; a non-NULL result means it was freed.
                    par = o[7] if len(o) > 7 else None
                    if par is not None and par in st.objs:
                        st.objs[par][0] = {LIVE}
            st.vars[name] = ("ptr", frozenset(), True, False)
        else:
            for oid in v[1]:
                o = st.objs.get(oid)
                if o is not None:
                    par = o[7] if len(o) > 7 else None
                    if par is not None and par in st.objs:
                        st.objs[par][0] = {FREED}
            st.vars[name] = ("ptr", v[1], False, v[3])
        return st

    def refine_int_cmp(self, st, op, l, r, want_true):
        st = st.copy()
        # try var op expr and expr op var
        for varside, otherside, flip in ((l, r, False), (r, l, True)):
            if varside[0] != "var":
                continue
            name = varside[1]
            v = st.vars.get(name)
            if v is None or v[0] != "int":
                continue
            ov = self.eval_pure(st, otherside)
            if ov[0] != "int":
                continue
            eff_op = op
            if flip:
                eff_op = {"<": ">", "<=": ">=", ">": "<", ">=": "<="}.get(op, op)
            iv = self.refine_interval(v[1], eff_op, ov[1], want_true)
            if iv is not None:
                st.vars[name] = int_val(*iv)
            return st
        return st

    def refine_interval(self, iv, op, oiv, want_true):
        (lo, hi) = iv
        (olo, ohi) = oiv
        # only handle the 'true' direction for inequalities; false branch
        # of < is >= etc handled by caller via want_true on same op? We
        # implement: if want_true is False, invert the operator.
        if not want_true:
            inv = {"<": ">=", "<=": ">", ">": "<=", ">=": "<",
                   "==": "!=", "!=": "=="}
            op = inv.get(op, op)
            if op in ("!=",):
                return iv
        if op == "==":
            nlo = lo if olo is None else (olo if lo is None else max(lo, olo))
            nhi = hi if ohi is None else (ohi if hi is None else min(hi, ohi))
            return (nlo, nhi)
        if op == "<":
            if ohi is None:
                return iv
            nhi = ohi - 1
            return (lo, nhi if hi is None else min(hi, nhi))
        if op == "<=":
            if ohi is None:
                return iv
            return (lo, ohi if hi is None else min(hi, ohi))
        if op == ">":
            if olo is None:
                return iv
            nlo = olo + 1
            return (nlo if lo is None else max(lo, nlo), hi)
        if op == ">=":
            if olo is None:
                return iv
            return (olo if lo is None else max(lo, olo), hi)
        return iv

    def eval_pure(self, st, expr):
        """Non-mutating evaluation for refinement; returns value or
        UNKNOWN. Only handles side-effect-free expressions."""
        tag = expr[0]
        if tag == "num":
            return self.eval_num(expr[1])
        if tag == "var":
            return st.vars.get(expr[1], UNKNOWN)
        if tag == "binary" and expr[1] in ("+", "-", "*", "&", "|", "^"):
            l = self.eval_pure(st, expr[2])
            r = self.eval_pure(st, expr[3])
            if l[0] == "int" and r[0] == "int":
                if expr[1] == "+":
                    return self.int_add(l[1], r[1])
                if expr[1] == "-":
                    nr = (None if r[1][1] is None else -r[1][1],
                          None if r[1][0] is None else -r[1][0])
                    return self.int_add(l[1], nr)
                if expr[1] == "*":
                    return self.int_mul(l[1], r[1])
            return INT_UNKNOWN if l[0] == "int" or r[0] == "int" else UNKNOWN
        if tag == "unary" and expr[1] in ("-", "+"):
            v = self.eval_pure(st, expr[2])
            if v[0] == "int":
                return self.int_unary(expr[1], v[1])
            return UNKNOWN
        if tag == "cast":
            return self.eval_pure(st, expr[2])
        if tag == "comma":
            return self.eval_pure(st, expr[1][-1])
        if tag == "sizeof":
            return int_val(*self._sizeof_interval(st, expr[1]))
        return UNKNOWN

    def eval_trunc(self, iv, type_str, line):
        # flag possible truncation only for explicit 32-bit casts; char/
        # short/byte conversions are too common to be useful signal
        if not re.search(r"\b(u?int32_t|u?int_least32_t|u?int_fast32_t|"
                         r"unsigned int)\b", type_str):
            return int_val(*iv)
        limit = (1 << 32) - 1
        lo, hi = iv
        if hi is not None and hi > limit:
            self.report("ALLOC_SIZE_TRUNCATION", "possible", line, 0,
                        "value may be truncated by 32-bit cast")
        return int_val(*iv)

    # ---------------------------------------------------------- transfer
    def initial_state(self):
        st = State()
        for name, ptr_depth, has_dims in self.func.params:
            if ptr_depth > 0 or has_dims:
                # incoming pointers are assumed valid (non-NULL) unless
                # the function itself establishes otherwise; this keeps
                # NULL_DEREF focused on locally-provable nullness
                st.vars[name] = ptr_val((), null=False, unk=True)
            else:
                st.vars[name] = INT_UNKNOWN
        return st

    def transfer(self, st, node):
        stmt = node.stmt
        tag = stmt[0]
        if tag == "eval":
            self.eval(st, stmt[1])
        elif tag == "decl":
            self.transfer_decl(st, stmt[1])
        elif tag == "return":
            if stmt[1] is not None:
                v = self.eval(st, stmt[1])
                if v[0] == "ptr":
                    for oid in v[1]:
                        o = st.objs.get(oid)
                        if o is not None:
                            o[3] = True
        elif tag == "opaque":
            # unanalyzable statement: unbind pointer vars, escape objects
            for name, v in list(st.vars.items()):
                if v[0] == "ptr":
                    self.escape_objs(st, v[1])
                    st.vars[name] = ("ptr", frozenset(), True, True)
        return st

    def transfer_decl(self, st, decls):
        for d in decls:
            name = d.name
            is_ptr = (d.ptr_depth > 0 or d.is_funcptr or d.is_function
                      or bool(d.dims))
            if is_ptr and not d.dims:
                if d.init == "complex":
                    st.vars[name] = ptr_val((), null=False, unk=True)
                elif d.init is not None:
                    v = self.eval(st, d.init)
                    if v[0] == "ptr":
                        st.vars[name] = v
                    elif v[0] == "addr":
                        st.vars[name] = ptr_val((), null=False, unk=True)
                    elif v[0] == "int" and d.ptr_depth > 0:
                        # int assigned to pointer (NULL/0 or cast)
                        if v[1][0] == 0 and v[1][1] == 0:
                            st.vars[name] = ptr_val((), null=True, unk=False)
                        else:
                            st.vars[name] = ptr_val((), null=False,
                                                    unk=True)
                    else:
                        st.vars[name] = ptr_val((), null=False, unk=True)
                else:
                    st.vars[name] = ptr_val((), null=False, unk=True)
            elif d.dims:
                # array variable: record first dim interval plus the
                # total byte size for sizeof()
                dim_iv = None
                if d.dims and d.dims[0] is not None \
                        and d.dims[0][0] == "num":
                    nv = self.eval_num(d.dims[0][1])
                    if nv[0] == "int":
                        dim_iv = nv[1]
                elif d.dims and d.dims[0] is not None:
                    ev = self.eval(st, d.dims[0])
                    if ev[0] == "int":
                        dim_iv = ev[1]
                st.vars[name] = ("array", (dim_iv,
                                           self._array_sizeof(st, d)))
                if d.init is not None and d.init != "complex":
                    self.eval(st, d.init)
            else:
                # scalar
                if d.init == "complex":
                    st.vars[name] = UNKNOWN
                elif d.init is not None:
                    v = self.eval(st, d.init)
                    if v[0] == "int":
                        st.vars[name] = v
                    elif v[0] == "ptr" and d.ptr_depth == 0:
                        st.vars[name] = UNKNOWN
                    else:
                        st.vars[name] = UNKNOWN
                    if d.init[0] == "call" and self._last_out_writes:
                        cname = self.callee_name(d.init[1])
                        if cname in self.out_handlers:
                            st.outmap[name] = frozenset(
                                self._last_out_writes)
                else:
                    st.vars[name] = INT_UNKNOWN
                if d.is_funcptr:
                    st.vars[name] = UNKNOWN

    def _array_sizeof(self, st, d):
        """Byte size of a fixed array declarator, or None when any dim or
        the element size is unknown."""
        if d.ptr_depth > 0 or d.is_funcptr:
            esize = 8
        else:
            esize = d.type_size
        if not esize or not d.dims:
            return None
        prod = (1, 1)
        for dim in d.dims:
            if dim is None:
                return None
            dv = self.eval(st, dim)
            if dv[0] != "int":
                return None
            prod = self.int_mul(prod, dv[1])[1]
            if prod[0] is None and prod[1] is None:
                return None
        return self.int_mul(prod, (esize, esize))[1]

    # ---------------------------------------------------------- leaks
    def check_leaks(self, st):
        self._cur_objs = st.objs
        for oid, o in st.objs.items():
            status, maybe, escaped, returned = o[0], o[1], o[2], o[3]
            if LIVE not in status or escaped or returned:
                continue
            conf = "definite" if (status == {LIVE} and not maybe) \
                else "possible"
            self.report("MEMORY_LEAK", conf, o[5] or 0, 0,
                        f"{self.obj_desc(oid)} may still be live at "
                        "function exit and is not returned or released",
                        self.trace_for(oid))

    # ---------------------------------------------------------- solve
    def widen(self, new, old):
        """Widening operator: if an integer interval bound grew relative to
        the previous joined state, make it unbounded. Guarantees fast
        convergence on loops like ``for (i = 0; i < n; i++)``."""
        for name, v in new.vars.items():
            if v[0] != "int":
                continue
            ov = old.vars.get(name)
            if ov is None or ov[0] != "int":
                continue
            (lo, hi), (olo, ohi) = v[1], ov[1]
            nlo, nhi = lo, hi
            if hi is not None and ohi is not None and hi > ohi:
                nhi = None
            if lo is not None and olo is not None and lo < olo:
                nlo = None
            if (nlo, nhi) != (lo, hi):
                new.vars[name] = int_val(nlo, nhi)

    def analyze(self, entry, nodes, exit_id, loop_heads=()):
        loop_heads = set(loop_heads)
        in_state = {entry: self.initial_state()}
        worklist = [entry]
        in_work = {entry}
        self._cur_objs = {}
        # Phase 1: compute the fixpoint without reporting. Intermediate
        # worklist states are incomplete (not all predecessors have
        # contributed yet), so findings collected there could be spurious.
        self._report_enabled = False
        iterations = 0
        while worklist and iterations < 10000:
            iterations += 1
            nid = worklist.pop()
            in_work.discard(nid)
            st = in_state.get(nid)
            if st is None:
                continue
            node = nodes[nid]
            out = self.transfer(st.copy(), node)
            for (succ, pol, cond) in node.succs:
                s = out
                if cond is not None and pol is not None:
                    s = self.refine(out.copy(), cond, pol == "true")
                cur = in_state.get(succ)
                if cur is None:
                    in_state[succ] = s
                    if succ not in in_work:
                        worklist.append(succ)
                        in_work.add(succ)
                else:
                    joined = cur.join(s)
                    if succ in loop_heads:
                        self.widen(joined, cur)
                    if joined.vars != cur.vars or joined.objs != cur.objs:
                        in_state[succ] = joined
                        if succ not in in_work:
                            worklist.append(succ)
                            in_work.add(succ)
        # Phase 2: replay every reachable node once with its converged
        # in-state to collect findings.
        self._report_enabled = True
        for nid, node in enumerate(nodes):
            st = in_state.get(nid)
            if st is None:
                continue
            self.transfer(st.copy(), node)
        # leak check at exits: return nodes and the synthetic exit node
        for nid, node in enumerate(nodes):
            if node.succs:
                continue
            st = in_state.get(nid)
            if st is None:
                continue
            out = self.transfer(st.copy(), node)
            self.check_leaks(out)
        return in_state
