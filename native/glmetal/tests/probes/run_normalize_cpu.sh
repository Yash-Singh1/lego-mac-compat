#!/bin/sh
# CPU-only: no driver library build and no GPU execution.
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-normalize.XXXXXX")
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
    tests/probes/normalize_cpu.cpp \
    third_party/install/lib/libglslang.a \
    third_party/install/lib/libMachineIndependent.a \
    third_party/install/lib/libGenericCodeGen.a \
    third_party/install/lib/libOSDependent.a \
    third_party/install/lib/libSPIRV.a \
    third_party/install/lib/libglslang-default-resource-limits.a \
    third_party/install/lib/libspirv-cross-msl.a \
    third_party/install/lib/libspirv-cross-glsl.a \
    third_party/install/lib/libspirv-cross-core.a \
    -o "$probe_work/normalize_cpu"
"$probe_work/normalize_cpu" "$probe_work"

while IFS= read -r probe_stage; do
    xcrun metal -std=macos-metal2.3 -c "$probe_work/$probe_stage" -o "$probe_work/$probe_stage.air"
done < "$probe_work/manifest.txt"
xcrun clang -ffp-contract=off -fno-fast-math tests/probes/normalize_reference_cpu.c -o "$probe_work/normalize_reference_cpu"
"$probe_work/normalize_reference_cpu"
for probe_lanes in 2 3 4; do
    xcrun metal -std=macos-metal2.3 -fno-fast-math -S -emit-llvm "$probe_work/normalize_ir_$probe_lanes.metal" -o "$probe_work/normalize_ir_$probe_lanes.ll"
done
python3 - "$probe_work" <<'PYCODE'
from pathlib import Path
import sys
for lanes in (2, 3, 4):
    source = (Path(sys.argv[1]) / f"normalize_ir_{lanes}.ll").read_text()
    assert "air.rsqrt.f32" in source, "Reciprocal-square-root call missing"
    assert "air.dot" not in source, "Normalization returned to native fused dot"
    assert "fma" not in source, "Normalization contracted into FMA"
    assert "fmul contract" not in source and "fadd contract" not in source, "Normalization arithmetic still permits contraction"
    print(f"vec{lanes} LLVM contains scalar products/sums and rsqrt without native dot or contraction")
PYCODE
