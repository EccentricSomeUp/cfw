<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0012: Cryptographic random source boundary

- Status: Accepted
- Date: 2026-11-25

## Context

Phase 2 requires a random-source boundary. Future subsystems need secure
randomness for request tokens, nonces, backoff jitter, and (potentially)
cryptographic material. A weak or biased source would silently undermine
security, so the API must guarantee OS CSPRNG quality and unbiased bounded
generation.

## Decision

Add `include/cfw/random.h` and `src/cfw/random.c`:

- `cfw_random_bytes(buffer, size)`: fills caller storage from the OS CSPRNG.
- `cfw_random_u64(value)`: fills a `uint64_t`.
- `cfw_random_below(bound, value)`: uniform value in `[0, bound)` via
  rejection sampling; `bound == 0` is `CFW_ERROR_INVALID_ARGUMENT`.

Backend selection is compile-time per platform:

- Linux/Android: `getrandom(2)` with EINTR retry, falling back to
  `/dev/urandom` (opened `O_RDONLY|O_CLOEXEC`) when getrandom is
  unavailable or restricted.
- macOS/BSD: `arc4random_buf`.
- Windows: `BCryptGenRandom` with `BCRYPT_USE_SYSTEM_PREFERRED_RNG`
  (links `bcrypt`; requests are capped at `ULONG` range).
- Other POSIX: `/dev/urandom`.

Partial reads are looped until `size` bytes are produced or an error
occurs. There is intentionally no seedable or deterministic mode.

`cfw_random_below` computes `remainder = 2^64 mod bound` (without forming
2^64) and accepts only candidates `<= UINT64_MAX - remainder`, making the
accepted range an exact multiple of `bound`; the result is `candidate %
bound`, which is unbiased. The rejection loop is capped at 64 attempts and
returns `CFW_ERROR_UNAVAILABLE` if exceeded (practically unreachable).

## Alternatives considered

- **`rand()`/`random()` PRNG with seed:** rejected; not cryptographically
  secure and seeding is a common source of predictable tokens.
- **OpenSSL or another dependency:** rejected; violates the
  dependency-free constraint, and OS CSPRNGs are sufficient.
- **Modulo reduction for bounded values:** rejected because it introduces
  modulo bias for non-power-of-two bounds.
- **Caller-provided entropy pool:** rejected; pushes CSPRNG correctness onto
  every consumer.

## Consequences

- Token, nonce, and jitter generation has a single audited source.
- Every call is a syscall; high-frequency callers should batch via
  `cfw_random_bytes` rather than repeated `cfw_random_below`.
- Platforms lacking all listed backends return `CFW_ERROR_UNAVAILABLE`.
- Security audits must cover backend selection, partial-read handling,
  rejection-sampling bounds, and the Windows size cap.
