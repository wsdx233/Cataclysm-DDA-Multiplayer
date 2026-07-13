#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
flatc="${FLATC:-flatc}"
expected_version='flatc version 1.12.0'
schema="$script_dir/multiplayer_protocol.fbs"
destination="$repo_root/src/third-party/multiplayer_protocol_generated.h"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT

actual_version="$($flatc --version)"
if [[ "$actual_version" != "$expected_version" ]]; then
  printf 'expected %s, got %s\n' "$expected_version" "$actual_version" >&2
  exit 1
fi

"$flatc" \
  --cpp \
  --scoped-enums \
  -o "$temporary" \
  "$schema"

generated="$temporary/multiplayer_protocol_generated.h"
if [[ "${1:-}" == '--check' ]]; then
  cmp "$generated" "$destination"
else
  cp "$generated" "$destination"
fi
