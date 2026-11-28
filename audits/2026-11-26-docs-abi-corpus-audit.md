<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: documentation/ABI catalog item + post-roadmap test additions

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: c5b2857
- Scope: catalog item 10 (documentation and ABI) from
  `docs/security/audit-catalog.md`, plus the changes landed after
  `2026-11-26-post-roadmap-hardening-audit.md`
- Method: API/ABI diff review, symbol inspection, documentation review,
  package consumption tests

## Catalog item 10 coverage

The 1.0-readiness audit table covered catalog items 1–9; this record
closes item 10 explicitly.

| Focus area | Result | Evidence |
|------------|--------|----------|
| Public headers and exported symbols | Pass | All `include/cfw/*.h` reviewed; `CFW_API` visibility macros unchanged; `log.h` atomic fields now map to `std::atomic` under C++ with `static_assert` size checks guarding layout parity — no C-side layout change |
| Version and ABI macros | Pass | `cfw_get_version`/`cfw_get_abi_version` unchanged; `CFW_ABI_VERSION` still 1; project version remains 0.1.0 (API not frozen, per `docs/api-stability.md`) |
| Ownership and thread-safety documentation | Pass | Header contracts reviewed; `log.h` field-mutation prohibition restated in the new comment block; platform matrix corrected to mark Windows unsupported rather than "Supported" |
| Migration notes for breaking changes | N/A pre-1.0 | No breaking change landed; `docs/api-stability.md` documents that breaking changes require a major version |
| Security assumptions in docs | Pass | `docs/architecture.md` rewritten to describe implemented subsystems instead of "pre-implementation" intent; threat-model and platform docs re-checked against `src/` |
| Package consumption tests | Pass (new) | `cfw_cpp_consumer` test compiles all public headers as C++; `cfw_install_smoke` installs to a scratch prefix and builds/runs a `find_package(cfw)` consumer |

## Changes audited

- `include/cfw/log.h`: C++ atomic-type mapping (additive, layout-preserving).
- `cmake/cfw_compile_options.cmake`: C-only warnings excluded from C++
  targets (build-system only, no ABI impact).
- `tests/cpp_consumer/`, `tests/install_smoke/`: new test targets.
- `tests/fuzz/corpus/` + `http_fuzz_corpus`: 26 adversarial inputs replayed
  through parser, chunked decoder, and server pipeline.
- `.gitattributes`: `tests/fuzz/corpus/** -text` so wire bytes (CRLF) are
  stored verbatim.
- `docs/architecture.md`, `docs/platform-matrix.md`: accuracy fixes.

## Findings

None. The `log.h` change is the only public-header modification; it does
not alter the C ABI (field types, order, and sizes are unchanged for C
consumers) and adds compile-time guards for the C++ spelling.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure    # 5/5 incl. cpp_consumer + install_smoke
cmake --preset release && ctest --preset release
cmake --preset sanitizers && ctest --preset sanitizers
cmake --preset shared && ctest --preset shared
```

All presets pass on the audited tree.
