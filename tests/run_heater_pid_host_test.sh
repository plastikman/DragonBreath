#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
tmp="${TMPDIR:-/tmp}/dragonbreath-dc-pid"
out="${TMPDIR:-/tmp}/dragonbreath-pb-heater-pid-test"

# Extract dependency origin, path and version from the firmware's dc_pid block.
# Do not cross into sibling entries (including slash-containing component names).
# Fail closed if the pin is absent, duplicated, malformed or points elsewhere.
dc_pid_fields="$(awk '
  /^  dc_pid:[[:space:]]*(#.*)?$/ {
    if (seen++) { duplicate = 1 }
    in_pid = 1
    next
  }
  in_pid && (/^  [^[:space:]]/ || /^[^[:space:]]/) { in_pid = 0 }
  in_pid && /^[[:space:]]+git:[[:space:]]*/ {
    sub(/^[[:space:]]+git:[[:space:]]*/, "")
    sub(/[[:space:]]*#.*/, "")
    gsub(/^[[:space:]]+|[[:space:]]+$/, "")
    git = $0
    git_count++
  }
  in_pid && /^[[:space:]]+path:[[:space:]]*/ {
    sub(/^[[:space:]]+path:[[:space:]]*/, "")
    sub(/[[:space:]]*#.*/, "")
    gsub(/^[[:space:]]+|[[:space:]]+$/, "")
    path = $0
    path_count++
  }
  in_pid && /^[[:space:]]+version:[[:space:]]*/ {
    sub(/^[[:space:]]+version:[[:space:]]*/, "")
    sub(/[[:space:]]*#.*/, "")
    gsub(/^[[:space:]]+|[[:space:]]+$/, "")
    version = $0
    version_count++
  }
  END {
    if (seen != 1 || duplicate || git_count != 1 || path_count != 1 || version_count != 1) exit 1
    print git
    print path
    print version
  }
' "$root/main/idf_component.yml")" || {
  echo "Missing or ambiguous dc_pid dependency in firmware manifest" >&2
  exit 1
}
dc_pid_git="$(printf '%s\n' "$dc_pid_fields" | sed -n '1p')"
dc_pid_path="$(printf '%s\n' "$dc_pid_fields" | sed -n '2p')"
dc_pid_version="$(printf '%s\n' "$dc_pid_fields" | sed -n '3p')"
if [ "$dc_pid_git" != "https://github.com/justinh-rahb/dragon-core.git" ]; then
  echo "Unexpected dc_pid Git source: $dc_pid_git" >&2
  exit 1
fi
if [ "$dc_pid_path" != "components/dc_pid" ]; then
  echo "Unexpected dc_pid component path: $dc_pid_path" >&2
  exit 1
fi
if ! printf '%s\n' "$dc_pid_version" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$'; then
  echo "Invalid dc_pid version in manifest (expected unquoted vX.Y.Z tag): $dc_pid_version" >&2
  exit 1
fi
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
