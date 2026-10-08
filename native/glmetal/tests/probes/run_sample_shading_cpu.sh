#!/bin/sh
# CPU-only: no driver library build and no GPU execution.
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-sample-shading.XXXXXX")
cleanup() {
    if [ "${GLM_PROBE_KEEP:-0}" = 1 ]; then
        printf 'Artifacts retained: %s\n' "$probe_work"
    else
        rm -rf -- "$probe_work"
    fi
}
trap cleanup EXIT HUP INT TERM
cd "$probe_root"
clang++ -std=c++17 -Ithird_party/install/include -Ithird_party/glslang -Isrc -Ibuild/gen \
    tests/probes/sample_shading_cpu.cpp \
    third_party/install/lib/libglslang.a \
    third_party/install/lib/libMachineIndependent.a \
    third_party/install/lib/libGenericCodeGen.a \
    third_party/install/lib/libOSDependent.a \
    third_party/install/lib/libSPIRV.a \
    third_party/install/lib/libglslang-default-resource-limits.a \
    third_party/install/lib/libspirv-cross-msl.a \
    third_party/install/lib/libspirv-cross-glsl.a \
    third_party/install/lib/libspirv-cross-core.a \
    -o "$probe_work/sample_shading_cpu"
"$probe_work/sample_shading_cpu" "$probe_work"
python3 - "$probe_work" <<'PY'
from pathlib import Path
import subprocess
import sys

work = Path(sys.argv[1])
manifest = (work / 'manifest.txt').read_text().splitlines()
assert len(manifest) == 12 and len(set(manifest)) == 12, 'Invalid stage manifest'
assert {p.name for p in work.glob('*.metal')} == set(manifest), 'Unexpected MSL artifact'
for name in manifest:
    source = work / name
    assert source.is_file() and source.stat().st_size, f'Missing stage: {name}'
    subprocess.run(['xcrun', 'metal', '-std=macos-metal2.3', '-c', str(source),
                    '-o', str(source.with_suffix('.air'))], check=True)
print(f'Offline Metal: {len(manifest)} stages passed')
PY
