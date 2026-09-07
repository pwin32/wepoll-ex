#!/usr/bin/env bash
# Preserve command and log-write failures while streaming combined output.
set -uo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 LOG COMMAND [ARG ...]" >&2
    exit 2
fi

log_path=$1
shift
"$@" 2>&1 | tee "$log_path"
