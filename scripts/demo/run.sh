#!/usr/bin/env bash
# Shared by the demos: run pm_exec, show the summary fields named in FIELDS, and compare the
# run's decision_hash_v2 with the expected one. Usage: run.sh EXPECTED_HASH "FIELDS" pm_exec-args...
cd "$(dirname "$0")/../.."
want=$1 fields=$2
shift 2
out=$(build/native/apps/pm_exec "$@" 2>&1 >/dev/null)
line=$(echo "$out" | grep "^exec")
[ -n "$line" ] || { echo "$out" | tail -3; exit 1; }
for f in $fields; do echo "$line" | grep -o "\b$f [^ ]*"; done
got=$(echo "$line" | grep -o "decision_hash_v2 [0-9a-f]*" | cut -d" " -f2)
if [ "$got" = "$want" ]; then echo "decision_hash_v2 $got (expected)"; else echo "decision_hash_v2 $got, expected $want"; exit 1; fi
