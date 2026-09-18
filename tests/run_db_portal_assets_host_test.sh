#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out=$(mktemp "${TMPDIR:-/tmp}/dragonbreath-assets-test.XXXXXX")
trap 'rm -f "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/db_portal/include" -I"$root/tests/stubs" \
  "$root/tests/db_portal_assets_host_test.c" \
  "$root/components/db_portal/db_portal_assets.c" -o "$out"
"$out"
