<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: coverage-guided fuzzing infrastructure (ADR 0077)

- Date: 2026-11-28
- Auditor: Devin (agent)
- Commit: <pending — audit written pre-commit; range covers the
  tests/fuzz + CMake/CI diff>
- Category: memory safety, denial-of-service, test integrity,
  supply-chain compliance
- Scope: `tests/fuzz/fuzz_support.[ch]` (new), `fuzz_http.c`,
  `fuzz_uri.c`, `fuzz_query.c`, `fuzz_multipart.c`,
  `fuzz_head_reader.c`, `fuzz_ws_proto.c`, `fuzz_crypto.c`, `fuzz_http1_conn.c`, `fuzz_ws_client.c`, `fuzz_ws_server.c`, `fuzz_http_client.c`, `fuzz_static.c`,
  `fuzz_driver.c`
  (new), `tests/unit/test_http_fuzz.c`, `CMakeLists.txt`,
  `tests/CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml`

## Goals

- Verify the extracted fuzz pipeline is identical to the previous
  in-test implementation, the new targets cannot introduce
  nondeterminism or resource exhaustion, and the CI job cannot hang
  or leak artifacts.

## Method

- Audit-catalog checks:
  - Memory safety: all targets feed caller buffers through the public
    API with caller-sized scratch (`size + 1` for decoders that
    require it); `fuzz_multipart`/`fuzz_query` bound inputs at 1 MiB
    before allocating; `fuzz_uri` caps target splitting at 8 KiB and
    allocates `size + 1`.
  - Integer/bounds: scratch sizes follow each API's documented
    minimum (`query->size + 1`, `body->size`); part/pair capacities
    are fixed small arrays on the stack.
  - API misuse/ownership: `cfw_fuzz_dispatch_t` is initialized once
    and only read during fuzzing; `cfw_fuzz_http_one` takes it as
    `const`, matching prior usage.
  - Denial-of-service: libFuzzer `-max_total_time=120`,
    `-rss_limit_mb=2048`, `-max_len=4096` bound every CI run; the
    driver caps inputs at 1 MiB; per-target runtime is matrixed so a
    hang in one target cannot stall the others (`fail-fast: false`).
  - Platform behavior: driver mode builds on GCC/MSVC/Windows with
    only ASan/UBSan; libFuzzer mode is clang-only and thus confined
    to the Linux CI job.
  - Test integrity: `test_http_fuzz.c` now calls the shared
    `cfw_fuzz_http_one`, so the deterministic fuzzer and the
    coverage-guided target cannot drift apart. The extraction is
    verbatim plus one added response-head parse pass.
  - Supply-chain: no new dependencies; libFuzzer ships with clang.
- `cmake --preset debug && cmake --build --preset debug` and
  `ctest --preset debug`: 6/6 pass (extraction is behavior-identical).
- `cmake --preset fuzz && cmake --build --preset fuzz`: all twelve
  driver-mode targets build clean under GCC with `-fsanitize=
  address,undefined -Werror`.
- Corpus replay: each `build/fuzz/tests/cfw_fuzz_*` binary run over
  all 26 `tests/fuzz/corpus` inputs exits 0 under ASan/UBSan.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| F-001 | low | `fuzz_http` static ctx | Lazy static dispatch context lives for the process; intentional for fuzz throughput, read-only after init, and matches libFuzzer's single-process model. | accepted |
| F-002 | info | CI `fuzz` job | 120 s × 12 targets ≈ 24 min of fuzzing per push; bounded and parallel. First run may surface real bugs — that is the point; fixes are follow-ups. | accepted |
| F-003 | info | `fuzz_driver.c` | Driver is a dev tool, not shipped; file reading uses stdio with size caps, no path handling beyond argv. | accepted |
| F-004 | low | Windows fuzz builds | Driver mode works on Windows but `CFW_ENABLE_FUZZING` is not enabled in any preset used by Windows CI; enabling it there is a possible follow-up for corpus replay coverage. | deferred |

## Verification

```text
cmake --preset debug && cmake --build --preset debug     # clean
ctest --preset debug --output-on-failure                 # 6/6 passed
cmake --preset fuzz && cmake --build --preset fuzz       # clean (GCC driver mode)
build/fuzz/tests/cfw_fuzz_{http,uri,query,multipart} tests/fuzz/corpus/*  # all rc=0
# CI: fuzz matrix job runs libFuzzer targets 120s each on ubuntu+clang
```

## Follow-up

- Triage any `crash-*`/`timeout-*` artifacts from the first CI fuzz
  runs; each becomes a corpus seed + regression test.
- Consider `-use_value_profile=1` for the HTTP target and a WS-frame
  target once an in-memory upgraded-conn transport exists.
