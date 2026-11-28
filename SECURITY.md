<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Security policy

`cfw` treats security as its highest-priority requirement. Security reports
should include enough detail to reproduce and assess the issue without exposing
unnecessary sensitive information.

## Reporting a vulnerability

Preferred: GitHub Private Vulnerability Reporting is enabled for this
repository (Security tab → "Report a vulnerability").

If private reporting is unavailable, contact the repository owner through the
channel associated with the repository and request a private security
conversation. Do not publish exploit details, proof-of-concept code, or
vulnerability analysis in public issues or pull requests before a fix or
coordination plan exists.

Reports should include, when possible:

- Affected version, commit, platform, and build configuration
- Minimal reproduction steps or proof of concept
- Expected and observed behavior
- Impact assessment, including attacker-controlled inputs and trust boundaries
- Any relevant logs or sanitizer output

## Scope

Security review covers all public APIs, internal memory management, parsers,
protocol state machines, platform abstraction layers, build configuration,
dependencies, and release artifacts.

## Supported versions

Only the latest released version and the current development branch receive
security fixes. Public API and ABI compatibility is governed by
`docs/api-stability.md`; the guarantees begin with the `1.0.0` release.

## Disclosure

Vulnerability details will be handled privately until a fix is available or a
coordinated disclosure date is agreed with the reporter. Credit will be given to
reporters unless they prefer otherwise.
