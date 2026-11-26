<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0054: portable maximum-alignment type (cfw_max_align_t)

## Status

Accepted.

## Context

Several public structs carry platform primitives in fixed inline byte
storage and need a union member that guarantees alignment for whatever
the platform puts there (`pthread_mutex_t`, `SRWLOCK`, `HANDLE`,
`pthread_t`, ...). The previous code used `max_align_t`, which is a
POSIX extension — not ISO C11 — and is absent from the MSVC headers.
That broke the Windows compile check on `cfw_mutex_t`, `cfw_cond_t`,
`cfw_thread_t`, and the arena's `_Alignof(max_align_t)` alignment
constant.

The project targets C11 only (AGENTS.md), so the alignment type must
come from the standard or be defined by the framework.

## Decision

New public header `cfw/max_align.h` defines:

```c
typedef union cfw_max_align {
    void *object_pointer;
    long long long_long_integer;
    long double long_double_float;
    void (*function_pointer)(void);
} cfw_max_align_t;
```

The union covers every fundamental scalar alignment on supported
platforms: object pointers, the widest integer, the widest floating
type, and function pointers. On platforms where `long double` is not
wider than `double` (MSVC) it still yields the platform's maximum
scalar alignment, which is all the storage-padding use case requires.

`cfw_max_align_t` replaces `max_align_t` in `cfw_mutex_t`,
`cfw_cond_t`, `cfw_thread_t`, and the arena alignment constant
(`_Alignof(cfw_max_align_t)`). On POSIX the resulting size and
alignment are identical to `max_align_t`, so there is no ABI change.

## Consequences

- Additive public header; no existing symbol changes layout.
- The arena continues to guarantee maximum fundamental alignment for
  every allocation; `cfw_static_mount_t` slot carving relies on this.
- MSVC C sources now compile with `/std:c11` so that `<stdatomic.h>`
  (used by `cfw/log.h`) exposes C11 atomic types; the flag is scoped
  to C via a `COMPILE_LANGUAGE` generator expression.
