# Security

Scope: local paper system, no live orders (D1). Assets: correctness of results, the laptop, recordings.

## Trust boundaries

- SEC1 Market feed (TLS WebSocket, exchange) is untrusted input: every message goes through the decoder that the property test and fuzzer cover; malformed -> counted, dropped; never crashes, never UB (asan/ubsan/fuzz gates).
- SEC2 Recordings on disk are untrusted input too (same decoder path).
- SEC3 HTTP server: binds 127.0.0.1 only (check at E4.5 with `ss -ltnp`). Read-only endpoints plus `POST /kill`. No endpoint changes risk limits, resets kill, or places/cancels orders other than kill's cancel-all.
- SEC4 Config file `exec.cfg`: parsed strictly; unknown key -> refuse to start; every limit has a hard upper bound in code (a typo cannot set capital to 1e12).
- SEC5 Gamma HTTP API (discovery) untrusted: ids and names validated (length, charset) before use in subscriptions, file names or HTML.

## Rules

- SEC6 No live order code: no order endpoint host, no signing, no key loading anywhere (D1).
- SEC7 No secrets in the repo: `.gitignore` gets `*.key`, `*.pem`, `.env`, `secrets/`, `*.secret` in E1.1.
- SEC8 Dashboard HTML escapes every exchange-provided string (labels, slugs) before insertion (existing `json_str`; check the HTML side uses textContent, not innerHTML, at E4.5).
- SEC9 Kill switch fails safe: any internal error on the trading thread -> trip kill, not continue (D11).
- SEC10 No `system()`, `popen()`, or shell-outs from any binary.
- SEC11 File writes only under the run directory given on the command line; paths built from exchange data are sanitised (no `/`, `..`, NUL).

## Gates (run at E7, and in CI after)

- SEC-G1 `grep -rnE "clob\.|/order|private_key|secp256k1|keccak|signTypedData" src apps` -> no hits.
- SEC-G2 `git ls-files | grep -E "\.(key|pem|env|secret)$"` -> empty.
- SEC-G3 `ss -ltnp` during a paper run shows the HTTP port on 127.0.0.1 only.
- SEC-G4 fuzz targets (itch, tape, oms) 60 s each clean in CI.
- SEC-G5 `grep -rnE "system\(|popen\(" src apps` -> no hits.
