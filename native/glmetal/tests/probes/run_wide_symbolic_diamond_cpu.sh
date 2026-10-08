#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
artifacts=$(mktemp -d "${TMPDIR:-/tmp}/glm-wide-symbolic.XXXXXX")
trap 'rm -rf "$artifacts"' EXIT HUP INT TERM
${CC:-clang} -O2 -Wall -Wextra -I"$root/tests/probes" \
  "$root/tests/probes/wide_symbolic_diamond_cpu.c" -o "$artifacts/probe"
"$artifacts/probe"
