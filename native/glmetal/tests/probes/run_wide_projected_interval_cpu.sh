#!/bin/sh
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glm-wide-projected.XXXXXX")
trap 'rm -rf "$probe_work"' EXIT HUP INT TERM
python3 - "$probe_root" "$probe_work/source.c" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]);source=(root/'src/vertex.c').read_text()
a=source.index('static bool wide_projected_interval(');b=source.index('static bool wide_line_draw(',a)
prefix='''#include <stdbool.h>
#include <math.h>
#include <float.h>
#define GLM_MAX_CLIP_PLANES 8
#define GLM_ATTR_POSITION 0
struct glm_state {int viewport[4];bool clip_plane_enabled[8];double clip_planes[8][4];};
struct glm_context {struct glm_state state;};
'''
pathlib.Path(sys.argv[2]).write_text(prefix+source[a:b]+(root/'tests/probes/wide_projected_interval_cpu.c').read_text())
PY
clang -O2 -fsanitize=undefined "$probe_work/source.c" -lm -o "$probe_work/probe"
"$probe_work/probe"
