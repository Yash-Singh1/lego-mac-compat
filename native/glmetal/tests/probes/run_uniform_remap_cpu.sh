#!/bin/sh
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-uniform-remap.XXXXXX")
trap 'rm -rf -- "$probe_work"' EXIT HUP INT TERM
cd "$probe_root"
python3 - "$probe_work" <<'PY'
from pathlib import Path
import sys
source=Path('src/metal_backend.m').read_text()
start=source.index('static size_t uniform_bytes(')
end=source.index('/* Geometry stage emulation',start)
Path(sys.argv[1],'uniform_remap_impl.h').write_text(source[start:end])
PY
clang -std=c11 -fsanitize=address,undefined -Isrc -Ibuild/gen -I"$probe_work" \
    tests/probes/uniform_remap_cpu.c -o "$probe_work/probe"
"$probe_work/probe"
