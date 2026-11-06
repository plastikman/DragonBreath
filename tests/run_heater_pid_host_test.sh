#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
tmp="${TMPDIR:-/tmp}/dragonbreath-dc-pid"
out="${TMPDIR:-/tmp}/dragonbreath-pb-heater-pid-test"

# Read the dc_pid pin from the same manifest that controls the firmware build.
# Fail closed if the entry is missing or the version is not a Git tag-like value.
dc_pid_version="$(awk '
  /^  dc_pid:/ { in_pid = 1; next }
  in_pid && /^  [[:alnum:]_]+:/ { exit }
  in_pid && /^[[:space:]]+version:[[:space:]]*/ {
    sub(/^[[:space:]]+version:[[:space:]]*/, "")
    gsub(/[[:space:]]/, "")
    print
    exit
  }
' "$root/main/idf_component.yml")"
case "$dc_pid_version" in
  v[0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "Invalid or missing dc_pid manifest pin: $dc_pid_version" >&2; exit 1 ;;
esac
base="https://raw.githubusercontent.com/justinh-rahb/dragon-core/$dc_pid_version/components/dc_pid"
mkdir -p "$tmp/include"
curl -fsSL "$base/include/dc_pid.h" -o "$tmp/include/dc_pid.h"
curl -fsSL "$base/dc_pid.c" -o "$tmp/dc_pid.c"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  -I"$root/tests/stubs" \
  -I"$tmp/include" \
  "$root/tests/pb_heater_pid_host_test.c" \
  "$tmp/dc_pid.c" \
  -lm -o "$out"

"$out"
