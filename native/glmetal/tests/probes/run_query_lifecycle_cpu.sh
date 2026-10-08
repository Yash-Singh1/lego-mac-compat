#!/bin/sh
# CPU-only query state regression, with address sanitization.
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-query-lifecycle.XXXXXX")
trap 'rm -rf -- "$probe_work"' EXIT HUP INT TERM
cd "$probe_root"
clang -fsanitize=address -g -Iinclude -Isrc -Ibuild/gen tests/probes/query_lifecycle_cpu.c -o "$probe_work/query_lifecycle_cpu"
"$probe_work/query_lifecycle_cpu"
