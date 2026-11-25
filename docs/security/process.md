<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Security process

Security is the highest-priority project requirement, followed by stability and
then extensibility. Security work is continuous rather than a release-time
checklist.

## Security principles

- Fail closed on invalid, ambiguous, or resource-exhausting input.
- Prefer explicit bounds and lengths over sentinel-terminated assumptions.
- Minimize attacker-controlled allocation sizes and amplification factors.
- Keep trust boundaries explicit: network input, configuration, filesystem
  input, callbacks, and plugin code are not automatically trusted.
- Do not log secrets, session tokens, authorization headers, or raw credentials.
- Treat timeouts, connection limits, request limits, and parser limits as core
  behavior.
- Security-sensitive code must be readable enough to audit line by line.

## Review triggers

The following always require security review:

- Public API or ABI changes
- Parsers, serializers, or protocol state machines
- Socket, event-loop, threading, or cancellation code
- Memory allocation, ownership, or lifetime changes
- Authentication, authorization, session, cookie, or TLS design
- Cryptographic primitives or protocol choices
- Dependency or toolchain changes
- Build, install, or packaging changes that affect shipped artifacts

## Audit cadence

- Run targeted audits for every security-relevant change.
- Run a broad audit before each minor release.
- Run a comprehensive audit before each major release and before 1.0.
- Re-run affected audits after fixes and record whether findings were resolved,
  accepted with rationale, or deferred.

Audit records should include scope, method, findings, severity, disposition,
and verification evidence.

## Severity guidance

Severity should consider exploitability, required privileges, impact, and
deployment context. At minimum classify findings as:

- Critical: remote code execution, authentication bypass, or severe memory
  corruption with practical exploitability
- High: memory corruption, denial of service, or security-control bypass with
  limited preconditions
- Medium: limited information disclosure, resource exhaustion, or behavior that
  violates security invariants
- Low: hardening gaps or defense-in-depth improvements
- Informational: documentation, clarity, or non-exploitable robustness issues

## Security test requirements

Security-sensitive code should have tests for malformed inputs, boundary
conditions, allocation failure where injectable, timeout/cancellation behavior,
and resource limits. Fuzzing should be added when parsers or protocol handling
are implemented.
