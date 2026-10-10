#!/usr/bin/env bash
# Static security gates (docs/exec/SECURITY.md SEC-G1, G2, G5). Exit 1 on any hit.
cd "$(dirname "$0")/.."
fail=0
gate() {
  local name=$1 hits=$2
  if [ -n "$hits" ]; then echo "$name FAILED:"; echo "$hits"; fail=1; else echo "$name ok"; fi
}
# G1: no order entry: trading API host, order endpoint paths, keys or signing (D1, SEC6).
gate SEC-G1 "$(grep -rnE '[/"]clob\.|"/orders?\b|private_key|secp256k1|keccak|signTypedData' src apps)"
# G2: no secrets tracked (SEC7).
gate SEC-G2 "$(git ls-files | grep -E '\.(key|pem|env|secret)$')"
# G5: no shell-outs (SEC10).
gate SEC-G5 "$(grep -rnE 'std::system\(|\bsystem\(\s*"|popen\(|\bexec[lv]p?e?\(' src apps)"
exit $fail
