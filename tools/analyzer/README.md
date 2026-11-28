# cfw_analyze — static memory-safety analyzer

A conservative, intraprocedural static analyzer for the C11 code in this
repository. It reads C source text and reports likely memory-safety bugs.

**It never compiles, links, imports, or executes any C code or project
script.** Parsing source text is not the same as compiling or testing the
program; findings are hypotheses from source-level control flow, not
proofs.

## Invocation

No dependencies beyond Python 3 (standard library only):

```sh
python3 -m tools.analyzer.cfw_analyze src/cfw          # human-readable
python3 -m tools.analyzer.cfw_analyze --json src/cfw   # JSON
python3 -m tools.analyzer.cfw_analyze tests/unit       # any path(s)
python3 -m tools.analyzer.cfw_analyze --include-generated src include tests
```

Exit status is 1 when findings are present (`--exit-zero` or
`--exit-status N` to change). The summary reports scanned files, analyzed
vs. partially-analyzed functions, skipped paths, and per-file partial
counts.

### Configuration

Allocator/release summaries can be extended on the command line:

- `--alloc-fn NAME` — function returning a newly allocated pointer
  (`malloc`-like; first argument is the size)
- `--calloc-fn NAME` — `calloc`-like (two size arguments, multiplied)
- `--realloc-fn NAME` — `realloc`-like
- `--free-fn NAME` — release function taking the pointer as first argument
- `--borrow-fn NAME` — function that borrows pointer arguments (reads or
  writes through them but takes no ownership)
- `--out-param NAME[:IDX]` — allocator that writes the result pointer
  through an argument (default index 2, i.e. `f(..., &out)`)
- `--define NAME[=VALUE]` — pretend `NAME` is defined for preprocessor
  conditionals (repeatable; `VALUE` may be a numeric constant expression)
- `--include-generated` — also read generated CMake headers under
  `build/*/include/cfw` for platform/configuration macro values

Built-in modeled APIs: `malloc`, `calloc`, `realloc`, `free`, `strdup`,
`strndup`, `valloc`, `aligned_alloc`, `posix_memalign`, and the project
wrappers `cfw_arena_create`, `cfw_arena_alloc`, `cfw_arena_alloc_zeroed`,
`cfw_arena_destroy`, `cfw_arena_reset`. Borrowed-argument checks cover the
common `mem*`/`str*`/`stdio` families.

## Analysis model

For each supported function the analyzer builds a control-flow graph
(branches, loops with back-edges, `goto`, `switch`, early returns) and runs
a forward flow-sensitive dataflow analysis over an abstract state:

- pointer variables → sets of allocation-site objects, may-be-null and
  may-be-unknown flags
- allocation objects → live/freed status (per path, joined as unions),
  allocation site/line, size interval when statically known, escape and
  returned flags
- simple integer intervals for sizes and indices (with widening at loop
  heads so fixpoint iteration terminates)

Joins union possible targets and statuses: an object freed on one path and
live on another is reported as *possible*, not definite.

### Rules

| Rule | Meaning |
|------|---------|
| `USE_AFTER_FREE` | deref/read/write/index through an object already freed |
| `DOUBLE_FREE` | release of an object that may already be freed |
| `MEMORY_LEAK` | object still live, owned, and not returned/escaped at exit |
| `NULL_DEREF` | dereference of a pointer that may be null |
| `OUT_OF_BOUNDS` | array index or copy length outside a known size |
| `ALLOC_SIZE_OVERFLOW` | allocation size expression may exceed `SIZE_MAX` |
| `ALLOC_SIZE_TRUNCATION` | value may be truncated by a 32-bit cast |

`definite` means the finding holds on every path represented by the
converged abstract state; `possible` means it holds on some paths.

### Conventions and assumptions

- Incoming pointer parameters and uninitialized pointer locals are assumed
  **non-NULL** unless the function itself establishes otherwise (e.g.
  `p = NULL`, a failed `malloc` check, or a `== NULL` branch that does not
  return). This keeps `NULL_DEREF` focused on locally provable nullness;
  it cannot see caller behavior.
- `malloc`/`calloc`/`realloc` results may be NULL; dereferencing them
  without a check is reported. `free(NULL)` is safe.
- `realloc` is modeled with success/failure outcomes: on success the old
  object is invalidated, on failure it stays live and the result is NULL.
  `p = realloc(p, n)` without a temporary is treated accordingly.
- Storing a pointer into unknown memory (struct members, arrays, calls
  without summaries) marks the object escaped — it is no longer reported
  as a leak.
- `return p` transfers ownership of `p`'s objects out of the function.
- Status-check correlation: after `if (f(...) != CFW_OK) return;` (or the
  `status = f(...); if (status != CFW_OK)` form) for a modeled
  out-parameter allocator, the written out-parameters are treated as
  non-NULL on the success path.

## Supported C subset and known limits

The parser is a deliberately bounded recursive-descent parser, not a full
C frontend. Supported: function definitions with prototype parameter
lists, local declarations (pointer depth, array dims, initializers),
compound statements, `if`/`else`, `while`, `do`/`while`, `for`,
`switch`, `return`/`break`/`continue`/`goto`/labels, calls, indexing,
member access, casts (including typedef/`*_t`/all-caps macro type names),
`sizeof`, `_Alignof`, ternary, comma expressions, compound literals,
block-scope `_Static_assert`, `_Alignas` declarations, adjacent string
literal concatenation (including object-like string macros and
encoding-prefixed literals), function-pointer and parenthesized
declarators (modeled as pointer-like values), block-scope typedefs and
function prototype declarations (used for name/cast disambiguation),
`va_arg` type-name operands plus `va_start`/`va_end`/`va_copy` calls,
declaration attributes such as `__attribute__`/`__declspec`, enum
constants (block- and file-scope, shared across the scan like typedefs),
and C11 integer constant macros such as `INT64_C`.

Preprocessor conditionals are filtered before parsing: `#if`/`#ifdef`/
`#ifndef`/`#elif`/`#else`/`#endif` are evaluated against C99/C11 integer
limits, host platform/compiler macros, numeric object-like `#define`s
(including transitive and cross-file references), generated CMake config
headers when `--include-generated` is used, and `--define` values.

Functions are marked *partially analyzed* (and listed in the summary)
when they use constructs the bounded evaluator cannot resolve, including:

- preprocessor conditionals whose value is still unknown after macro
  resolution — unknown branches are kept and reported rather than guessed
- variably-modified (VLA) types
- statement expressions, `_Generic`, inline `asm`, K&R definitions,
  nested functions, computed goto, and other unsupported constructs;
  these are skipped conservatively and reported instead of making the
  whole function opaque when possible

Other blind spots:

- no interprocedural analysis: callees without summaries are assumed to
  escape pointer arguments but not to free them; return values of unknown
  functions are unknown
- no points-to through structs: `a->f = p` escapes `p` but `a->f` reads
  back as unknown
- no aliasing precision beyond direct assignment (`q = p`, `&x` out-params)
- no concurrency reasoning, no integer wraparound modeling beyond interval
  bounds, no floating-point reasoning
- macro expansion is limited to object-like numeric/string constants used
  for preprocessor conditionals, constant expressions, array bounds, and
  string literals, plus C11 integer constant macros (`INT64_C`, `UINT32_C`,
  ...) and simple casts in constant expressions; function-like macros and
  non-constant expressions remain opaque
- findings are per-function; cross-function lifetime bugs (e.g. freeing in
  another function) are out of scope

If the subset proves insufficient for a file, a compiler-frontend mode
(e.g. `clang -Xclang -ast-dump` or `pycparser` with a real preprocessor)
would be the natural future enhancement — it is intentionally not part of
this implementation.

## Tests

```sh
python3 -m unittest tests.analyzer.test_cfw_analyze -v
```

Tests analyze small in-memory C fixture strings; they never compile or
execute C code.
