#!/bin/sh
# No driver build or GPU execution. Compile the real feedback.c with CPU mocks.
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-tf-object-state.XXXXXX")
trap 'rm -rf -- "$probe_work"' EXIT HUP INT TERM
cd "$probe_root"
clang -Iinclude -Isrc -Ibuild/gen tests/probes/tf_object_state_cpu.c -o "$probe_work/tf_object_state_cpu"
"$probe_work/tf_object_state_cpu"
