<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Fuzz corpus

`corpus/` contains hand-written adversarial HTTP/1.1 inputs replayed by the
`http_fuzz_corpus` unit test (`tests/unit/test_http_fuzz.c`). Each file is
fed verbatim through the request-head parser, framing logic, chunked
decoder, and the full `cfw_http1_server` pipeline; any crash or sanitizer
report is a test failure.

The corpus complements — it does not replace — the deterministic mutation
fuzzer (`http_fuzz_mutation`), which mutates embedded seeds with a
fixed-seed PRNG.

The same directory is also the seed corpus for the coverage-guided
libFuzzer targets (`fuzz_http`, `fuzz_uri`, `fuzz_query`,
`fuzz_multipart`, `fuzz_head_reader`,
`fuzz_ws_proto`, `fuzz_crypto`,
`fuzz_http1_conn`, `fuzz_ws_client`,
`fuzz_ws_server`, `fuzz_http_client`,
`fuzz_static`; ADR 0077, built with `CFW_ENABLE_FUZZING=ON`).
Crashes found by the fuzzer should be added here as regression seeds.

When adding entries:

- Use descriptive `NN-name.http` filenames so ordering and intent are clear.
- Cover parser edge cases: ambiguous framing (`Content-Length` vs
  `Transfer-Encoding`), obs-fold, bare-LF line endings, chunk extensions
  and trailers, malformed chunk sizes, oversized length digits, pipelined
  requests, upgrade requests, and truncated inputs.
- Keep files small (the test reads at most 8 KiB per file).
- Inputs may contain arbitrary bytes, including NULs.
