<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0077: Coverage-guided fuzz targets

## Status

Accepted.

## Context

The HTTP parser surface has been protected by a deterministic
mutation fuzzer (`http_fuzz_mutation`, fixed-seed xorshift64) and a
hand-written adversarial corpus replay (`http_fuzz_corpus`). Both run
inside `ctest` under ASan/UBSan, but neither is coverage-guided:
mutation is unguided random search, and corpus replay only re-checks
known-bad inputs. The parsers (request/response heads, chunked
coding, URI/query decoding, multipart) are exactly the kind of
stateful byte-munchers where coverage guidance finds bugs random
mutation misses.

## Decision

Add libFuzzer targets under `tests/fuzz/` behind a new opt-in CMake
option `CFW_ENABLE_FUZZING` (default OFF):

- `fuzz_support.[ch]` holds the fuzz pipeline previously embedded in
  `test_http_fuzz.c` (dispatch context + `cfw_fuzz_http_one`), so the
  deterministic fuzzer and the coverage-guided target exercise the
  identical code path. `cfw_fuzz_http_one` also gained a cheap
  response-head parse pass over the same input.
- Targets: `fuzz_http` (head/framing/dispatch/chunked/http1_server),
  `fuzz_uri` (percent-decoder, target path/query split, query pairs),
  `fuzz_query`, `fuzz_multipart` (boundary extraction + body parse).
  All operate on in-memory buffers through the public API — no
  sockets, threads, or filesystem — so they are fully deterministic
  and platform-independent.
- Toolchain split: Clang builds link `-fsanitize=fuzzer` (libFuzzer
  provides `main`); GCC/MSVC/Windows builds compile `fuzz_driver.c`,
  a standalone `main` that replays input files/stdin through the same
  `LLVMFuzzerTestOneInput` entry point under ASan/UBSan. This gives
  corpus replay and manual mutation fuzzing everywhere, and real
  coverage-guided fuzzing wherever clang is available.
- New `fuzz` CMake preset (`build/fuzz`, RelWithDebInfo +
  `CFW_ENABLE_FUZZING=ON`) and a CI `fuzz` job: matrix over the four
  targets on `ubuntu-latest` with `CC=clang`, each running
  `-max_total_time=120 -max_len=4096` against `tests/fuzz/corpus` as
  the seed corpus, artifacts to `build/fuzz/artifacts/`.
- `fuzz_http` keeps its dispatch context in a lazily-initialized
  static (arena allocated once per process) because libFuzzer invokes
  the entry point millions of times; per-input setup would dominate
  runtime. The context is immutable after init, matching the
  read-only usage in `cfw_fuzz_http_one`.

## Consequences

- Parser changes get continuous coverage-guided fuzzing in CI (8
  target-minutes per push) in addition to the deterministic suite.
- New public-API parsing surfaces should get a `fuzz_*` target or be
  folded into an existing one; private-only parsers may link their
  `.c` directly like the pollset compile fixtures do.
- Crashes found by the fuzzer land as `crash-*` artifacts; the
  reproducer is a single input file usable by both libFuzzer and the
  standalone driver.
- The deterministic mutation fuzzer and corpus replay are unchanged —
  they remain the always-on, every-platform check.
