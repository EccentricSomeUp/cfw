<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Testing roadmap

This document records aspirational directions for the test suite beyond the
current taxonomy in `docs/process/testing.md`. Nothing here is a commitment:
every item is a candidate that requires its own planning, audit, and ADR where
it changes build or CI policy. The purpose is to keep the ambition explicit —
the suite exists to make `cfw`'s security claims falsifiable, not merely
asserted.

Status anchors: the suite today has a custom C harness, unit/integration/
security binaries, ASan+UBSan and TSan presets, libFuzzer targets with a
replay corpus, backend compile fixtures, a FreeBSD QEMU CI job, Windows ctest,
install-smoke, and C++-consumer tests. Items below extend that foundation.

## Near-term — deepen what exists

### Fuzzing maturity

- **Dedicated CI job.** Move fuzzing from opt-in preset to a CI job that runs
  libFuzzer with a time budget on Clang, auto-merges new corpus entries back
  into `tests/fuzz/corpus/` with provenance notes, and reports coverage.
  Add a nightly long-run job on self-hosted or OSS-Fuzz-style infrastructure.
- **Dictionaries and structured mutators.** Raw byte mutation wastes budget
  re-discovering `\r\n`, chunked framing, and WebSocket opcodes. Provide
  libFuzzer dictionaries per target plus custom mutators that splice valid
  grammar fragments — valid prefixes with mutated tails reach deeper parser
  states faster.
- **Per-target coverage reporting.** `-print_coverage` / `llvm-cov` over the
  corpus run to identify unreached parser states, then write targeted seeds
  for those states.
- **Differential fuzzing — highest-value upgrade.** Run the same bytes
  through `cfw` and a reference parser (h11, llhttp, Python `http.client`)
  and flag divergent accept/reject decisions. For a smuggling-critical
  codebase, divergence between two independent interpretations *is* a bug
  candidate by definition.
- **Fuzz the transport seam.** TLS-adjacent state, ALPN negotiation results,
  SNI handling — currently TLS paths go through real OpenSSL, which caps
  fuzz throughput. Consider a deterministic fake-TLS transport backend for
  fuzzing the state machine above the crypto layer.

### Deterministic fault injection

- **Chaos integration suite.** Drop packets, RST mid-handshake, partial
  writes, EAGAIN storms, half-close, pipelined-then-abort, slowloris,
  dribble bodies. Most is scriptable over loopback with `SO_LINGER`,
  `TCP_NODELAY` toggling, and non-blocking writes — no external dependencies.
- **Resource-ceiling assertions as tests.** N concurrent connections,
  oversized heads, infinite chunked streams: assert peak RSS and fd count
  stay under documented bounds. The caps already exist; make the *bounds
  themselves* regression-tested so a refactor cannot silently raise them.
- **TLS battery.** Expired/wrong-SNI/mismatched certs, TLS version matrix,
  session-resumption races, handshake floods, certificate rotation
  mid-connection, `openssl s_client` / `testssl.sh` interop in a TLS CI job.

### Test-strength measurement

- **Mutation testing.** Mull (LLVM) or `mutagen`-style source mutators:
  measure the fraction of injected bugs the suite kills. For critical
  infrastructure this answers "are the tests actually testing?" — a
  per-module kill-rate dashboard is far more honest than line coverage.
- **Branch coverage gates.** `llvm-cov`/gcov report job with per-module
  thresholds that ratchet upward over time.
- **Audit-catalog coverage matrix.** Each entry in
  `docs/security/audit-catalog.md` maps to named tests; a CI check flags
  audits with zero linked tests. Extends the existing audit culture into a
  measurable property.

### Portability hardening

- **Cross-compile legs.** aarch64 Linux, possibly riscv64 or Android NDK —
  compile + unit-test under QEMU for at least one non-x86 target.
- **Compiler matrix.** GCC and Clang version spread, MSVC `/W4`-equivalent
  strictness. Werror is already on; extend the matrix so a warning on an
  older compiler does not silently rot.
- **ABI drift tests.** `abi-compliance-checker` or symbol-map diff against
  the last release tag, since ABI stability is a stated guarantee.

## Mid-term — the "incredibly thorough" layer

### Property-based and metamorphic testing

- **In-tree generative framework.** C has no Hypothesis; build a seeded RNG
  plus shrinker on top of `cfw_random`. Generate random spans, headers, URIs,
  and chunked bodies; assert invariants: encode→decode round-trips,
  canonicalization idempotence, and "two parsers disagree ⇒ bug" oracles.
  Deterministic seeds and recorded corpora per `docs/process/testing.md`.
- **Request-smuggling oracle — the killer property.** Generate a request
  and assert that *every* internal interpretation layer (front-end framing,
  route matching, body reader, any proxy path) agrees on where the request
  ends. Divergence is a smuggling bug by definition.
- **Stateful event-sequence fuzzing.** Not just bytes-in but sequences of
  operations on the connection object: send head, half-body, RST, new
  request on the same connection, pipelined requests, upgrade attempts
  mid-stream. Fuzz the event sequence, not only the buffer.

### Simulation testing

Already listed in `docs/long-term-vision.md`; make it concrete.

- **Virtual clock + virtual network harness.** Deterministic scheduler where
  every timeout race is testable by construction. Fuzz the scheduler seed to
  enumerate interleavings of accept/read/timeout/shutdown — turning "we
  tested shutdown once" into "we tested ~10⁶ shutdown orderings."
- This is how graceful-shutdown, fd-leak, and cancellation logic gets real
  confidence without flaky timing tests.

### Conformance and differential oracles

- **HTTP/1.1 conformance battery** derived from RFC 9112 edge cases —
  essentially a written-down h1 spec. Cross-check against `nghttp2`/h2spec-
  style tooling wherever applicable.
- **Differential server testing.** Replay identical client traffic against
  `cfw` and a reference server (nginx, h2o, or a Python asyncio server) and
  compare observable behavior on malformed inputs.
- **Real-traffic corpus.** Harvest anonymized HAR files / pcaps as fuzz
  seeds and replay fixtures. The gap between synthetic seeds and production
  traffic is where parser bugs live.

### Soak and regression infrastructure

- **Long-running soak job** (hours, not seconds): memory-leak detection over
  connection churn, fd-leak counting across restart cycles, TLS
  session-cache growth bounds.
- **Performance-budget tests.** `cfw_bench` already exists — turn
  latency/throughput regressions into nightly ctest gates with
  noise-tolerant thresholds rather than per-commit flake.
- **Flake management.** Track per-test pass rates over time; statistically
  auto-quarantine flaky tests so they do not erode trust in the suite.

## Far-future / differentiators

- **Formal verification.** The parsers are explicit state machines — nearly
  ideal for CBMC/KLEE bounded model checking. Machine-checked proofs that
  the HTTP/1.1 parser and chunked decoder cannot desynchronize is a claim
  no mainstream C framework makes. See `docs/long-term-vision.md`.
- **Emulated and exotic platforms.** QEMU for ARM/RISC-V, Valgrind as a
  second memory checker, Wine or real Windows VMs in a nightly matrix,
  illumos/NetBSD for `kqueue`/`poll` coverage beyond macOS/FreeBSD.
- **Distributed fuzzing at scale.** libFuzzer merge jobs across many cores,
  corpus federation, crash deduplication and triage feeding straight into
  `audits/`.
- **Continuous verification / canary testing.** Post-release, run the
  security and fuzz-replay suites continuously against released artifacts —
  extending the audit ledger so every release tag links to a green
  test-evidence hash.
- **Agent-era testing.** Structured machine-readable test/audit results
  (extending the status-code and audit-schema work) so automated reviewers
  and agents consume assurance evidence programmatically. LLM-assisted
  generation of adversarial test cases is promising but needs human review
  to avoid garbage-in tests.
- **Chaos-grade production-like harness.** Full virtual-network simulation
  with packet loss, jitter, and reordering as a first-class test dimension —
  "network condition fuzzing" for the whole server, not just parsers.

## Targeted test selection

Full-suite runs are correct for releases but wasteful for iterative agent or
human change loops. Support targeted runs at three levels:

1. **Per-binary filters.** The harness already supports `--filter`; extend
   naming so audit-linked tests are greppable by catalog entry (e.g.
   `cfw_security_tests --filter smuggling`).
2. **Change→test mapping.** A manifest (or generated file) mapping source
   paths to the tests that exercise them — e.g. edits under `src/cfw/http*`
   trigger unit + security + the relevant fuzz replay, while `src/cfw/ws*`
   pulls the websocket suites. Agents and developers run only the mapped
   subset; CI runs everything.
3. **Preset tiers.** A `quick` build/test tier (unit + security only, no
   TLS, no fuzz replay) for inner-loop iteration; `full` remains the release
   gate. Document the tier commands in `docs/process/testing.md`.

The goal is that a change to one subsystem can be verified in seconds-to-
minutes rather than a 10+ minute full-suite run, while release and audit
workflows still run everything.

## Suggested sequencing

1. **First:** fuzz CI job + corpus merge, differential parser oracle,
   chaos/fault-injection suite, resource-ceiling tests — highest
   bug-finding-per-effort.
2. **Then:** mutation testing + branch-coverage gates + audit-catalog test
   matrix — proves the suite's worth and prevents decay.
3. **Then:** deterministic simulation harness — biggest engineering lift,
   biggest payoff for concurrency/shutdown confidence.
4. **Ongoing:** CBMC on the wire parsers and real-traffic corpus harvesting
   as the differentiating long game.
