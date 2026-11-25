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

The initial harness lives in `tests/unit/test_harness.h` and
`tests/unit/test_main.c`. Test suites are registered explicitly in
`test_main.c`. Future suites may be split into separate executables or test
binaries while retaining the custom assertion and reporting conventions.

Useful commands:

```sh
./build/debug/tests/cfw_unit_tests --list
./build/debug/tests/cfw_unit_tests --filter status
ctest --preset debug --output-on-failure
ctest --preset sanitizers --output-on-failure
```

## Coverage and sanitizer expectations

- Debug builds are the default correctness configuration.
- Sanitizer builds enable ASan and UBSan where supported.
- Coverage measurement may be added later without external test dependencies.
- Every memory-safety-sensitive module should have sanitizer and fuzz coverage
  proportional to parser complexity and exposure.
