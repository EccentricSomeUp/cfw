<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0016: HTTP/1.x token primitives

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 3 begins with HTTP. Before request parsing, the framework needs
canonical representations and parsers for the three token families that
appear in every message: methods, versions, and status codes.

## Decision

Add `include/cfw/http.h` and `src/cfw/http.c`:

- `cfw_http_method_t`: the nine RFC 9110 methods plus UNKNOWN.
  `cfw_http_method_parse` matches tokens byte-exactly (methods are
  case-sensitive per RFC); unrecognized tokens yield
  `CFW_HTTP_METHOD_UNKNOWN` with `CFW_OK` so servers can answer 405/501
  rather than fail parsing.
- `cfw_http_version_t {major, minor}`: parsed from `HTTP/D.D` with
  strict grammar — no leading zeros, bounded digits, full-token match.
  `cfw_http_version_format` renders into a caller buffer (no shared
  static state).
- `cfw_http_status_t`: the standard status codes the framework emits or
  must distinguish, each with a `cfw_http_status_reason` phrase.
  `cfw_http_status_parse` maps numeric codes to the enum, rejecting codes
  outside the supported set.

All parsers take `cfw_strview_t` and never read past view bounds.

## Alternatives considered

- **Case-insensitive method matching:** rejected; RFC 9110 methods are
  case-sensitive and servers must not normalize attacker-controlled tokens.
- **Returning parse errors for unknown methods:** rejected; UNKNOWN is a
  valid parse outcome that the server layer maps to an error response.
- **Static string buffer for version formatting:** rejected for thread
  safety; caller-provided buffers keep the API reentrant.
- **Full numeric status range:** deferred; only codes the framework
  actually emits are enumerated, keeping the ABI surface tight.

## Consequences

- Request-line and response serialization have canonical tokens to build
  on.
- Grammar strictness (leading zeros, trailing garbage) is enforced at the
  token layer, reducing parser ambiguity for the message parser.
- Audit catalog 3 (parser/protocol) applies to all subsequent HTTP parsing.
