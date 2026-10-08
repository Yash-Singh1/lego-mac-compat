#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
artifacts=$(mktemp -d "${TMPDIR:-/tmp}/glm-wide-terminal.XXXXXX")
trap 'rm -rf "$artifacts"' EXIT HUP INT TERM
python3 - "$root" "$artifacts/source.c" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]);s=(root/'src/vertex.c').read_text()
a=s.index('static bool wide_terminal_pixel(');b=s.index('static bool wide_interval_state(',a)
pathlib.Path(sys.argv[2]).write_text('#include <stdbool.h>\n#include <stdint.h>\n#include <limits.h>\n#include <math.h>\n'+s[a:b]+(root/'tests/probes/wide_terminal_cpu.c').read_text())
PY
clang -O2 -I"$root/tests/probes" "$artifacts/source.c" -lm -o "$artifacts/probe"
"$artifacts/probe"
