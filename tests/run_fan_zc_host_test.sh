#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-fan-zc-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_fan/include" \
  "$root/components/pb_fan/pb_fan_zc_filter.c" \
  "$root/tests/pb_fan_zc_host_test.c" \
  -o "$out"

"$out"
