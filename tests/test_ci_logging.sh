#!/usr/bin/env bash
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -r -- "$test_dir"' EXIT

status=0
bash "$repo_dir/scripts/run-logged.sh" "$test_dir/failure.log" \
    bash -c 'printf "stdout\n"; printf "stderr\n" >&2; exit 17' || status=$?
[ "$status" -eq 17 ]
[ "$(wc -l < "$test_dir/failure.log")" -eq 2 ]
[ "$(sed -n '1p' "$test_dir/failure.log")" = stdout ]
[ "$(sed -n '2p' "$test_dir/failure.log")" = stderr ]

bash "$repo_dir/scripts/run-logged.sh" "$test_dir/success.log" \
    bash -c 'printf "%s\n" "$1"' bash 'argument with spaces'
[ "$(< "$test_dir/success.log")" = 'argument with spaces' ]

status=0
bash "$repo_dir/scripts/run-logged.sh" "$test_dir/missing/output.log" \
    bash -c 'exit 0' || status=$?
[ "$status" -ne 0 ]
