#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
artifacts=$(mktemp -d "${TMPDIR:-/tmp}/glm-wide-axis.XXXXXX")
trap 'rm -rf "$artifacts"' EXIT HUP INT TERM
clang -O2 -I"$root/tests/probes" "$root/tests/probes/wide_axis_interval_cpu.c" -lm -o "$artifacts/probe"
"$artifacts/probe"
python3 - "$root" "$artifacts/production.c" <<'PYCODE'
import pathlib,sys
root=pathlib.Path(sys.argv[1]);s=(root/'src/vertex.c').read_text()
a=s.index('struct wide_axis_interval {');b=s.index('static bool wide_terminal_state(',a)
test=(root/'tests/probes/wide_axis_interval_cpu.c').read_text().replace('#include "wide_axis_interval.h"','').replace('glm_probe_wide_axis_interval','wide_axis_bounds').replace('glm_probe_axis_interval','wide_axis_interval')
pathlib.Path(sys.argv[2]).write_text('#include <stdbool.h>\n#include <stdint.h>\n#include <limits.h>\n#include <math.h>\n'+s[a:b]+test)
PYCODE
clang -O2 "$artifacts/production.c" -lm -o "$artifacts/production"
"$artifacts/production"
