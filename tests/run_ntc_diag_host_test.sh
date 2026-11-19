#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-ntc-diag-test"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_ntc/include" \
  -I"$root/components/pb_ntc" \
  -I"$root/tests/stubs" \
  "$root/tests/pb_ntc_diag_host_test.c" -o "$out"
"$out"
