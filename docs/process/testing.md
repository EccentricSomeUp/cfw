<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Testing strategy

`cfw` uses a custom C test suite rather than an external test framework. CTest is
used only as a portable test runner and does not provide test semantics.

## Test taxonomy

### Unit tests

Purpose: verify individual functions, state machines, parsers, encoders, and
utility logic in isolation.

Location: `tests/unit`

Requirements:

- Deterministic and fast
- No network or filesystem dependence unless explicitly mocked internally
- Cover success, invalid input, boundary values, and error paths

### Integration tests

Purpose: verify interactions between modules such as server lifecycle, router,
request handling, and response generation.

Location: `tests/integration`

Requirements:

- Use loopback or in-process transports where possible
- Avoid flaky timing assumptions
- Clean up sockets, threads, timers, and temporary files reliably

### Security tests

Purpose: verify resistance to malformed input, resource exhaustion, parser
differentials, API misuse, and unsafe edge cases.

Location: `tests/security`

Requirements:

- Treat test inputs as attacker-controlled
- Include malformed, truncated, oversized, and ambiguous inputs
- Verify fail-closed behavior and bounded resource usage

### Regression tests

Purpose: preserve fixes for previously discovered bugs and audit findings.

Requirements:

- Name tests after the defect or audit finding where practical
- Include enough context to understand the original failure mode
- Must fail on the pre-fix implementation when practical

### Fuzz tests

Purpose: explore parser and protocol state spaces with generated inputs.

Requirements:

- Deterministic seeds and reproducible corpus handling
- Sanitizer-compatible builds
- Bounded runtime and resource usage
- Crash artifacts minimized before committing regression cases

### Property tests

Purpose: verify invariants across generated inputs, such as parser/serializer
round trips or bounds preservation.

Requirements:

- Document the property being tested
- Use deterministic generation or recorded seeds
- Avoid assertions that only test the implementation against itself

## Current harness

The harness lives in `tests/unit/test_harness.h` and is shared by three
test binaries: `cfw_unit_tests` (unit suites, registered in
`tests/unit/test_main.c`), `cfw_integration_tests` (end-to-end pipeline
over real loopback TCP), and `cfw_security_tests` (request-smuggling and
framing-abuse regressions). A fourth binary, `cfw_cpp_consumer`, is a
standalone C++ header/link smoke test built when a C++ compiler is
available.

Useful commands:

```sh
./build/debug/tests/cfw_unit_tests --list
./build/debug/tests/cfw_unit_tests --filter status
./build/debug/tests/cfw_integration_tests
./build/debug/tests/cfw_security_tests
ctest --preset debug --output-on-failure
ctest --preset sanitizers --output-on-failure
```

`cfw_unit_tests` also replays the adversarial corpus in `tests/fuzz/corpus`
(see `tests/fuzz/README.md`) and, when a C++ compiler is available, builds a
`cfw_cpp_consumer` test proving the public headers compile as C++. The
`cfw_install_smoke` CTest case installs into a scratch prefix and builds a
`find_package(cfw)` consumer against it. `cfw_pollset_poll_tests` runs the
pollset suite against the `poll()` fallback backend on every non-Windows
build via the test-only `CFW_POLLF_BACKEND=poll` override, so the non-epoll
path is verified even on Linux. Object-only compile fixtures additionally
force the `epoll`, `kqueue` (against a stub `<sys/event.h>` on hosts
without one), `poll`, and `none` backend selections so every `#if` branch
in `pollset.c` is compiled on every host (ADR 0053).

A manual benchmark binary, `cfw_bench` (POSIX builds only, not a CTest
case), measures single-connection keep-alive throughput against a live
`cfw_server_t`: `./build/release/tests/cfw_bench [requests] [warmup]`.
Use it for relative comparisons between builds on the same machine.

## Continuous integration

`.github/workflows/ci.yml` runs every configure preset (debug, release,
sanitizers, shared) on Ubuntu and macOS. The macOS leg exercises the
native `kqueue` backend that Linux's `epoll` selection never touches;
the `poll()` fallback is covered on every POSIX host by
`cfw_pollset_poll_tests`.

## Coverage and sanitizer expectations

- Debug builds are the default correctness configuration.
- Sanitizer builds enable ASan and UBSan where supported.
- Coverage measurement may be added later without external test dependencies.
- Every memory-safety-sensitive module should have sanitizer and fuzz coverage
  proportional to parser complexity and exposure.
