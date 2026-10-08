#!/bin/sh
# CPU-only: no driver library build and no GPU execution.
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-varying-limits.XXXXXX")
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
    tests/probes/varying_limits_cpu.cpp \
    third_party/install/lib/libglslang.a \
    third_party/install/lib/libMachineIndependent.a \
    third_party/install/lib/libGenericCodeGen.a \
    third_party/install/lib/libOSDependent.a \
    third_party/install/lib/libSPIRV.a \
    third_party/install/lib/libglslang-default-resource-limits.a \
    third_party/install/lib/libspirv-cross-msl.a \
    third_party/install/lib/libspirv-cross-glsl.a \
    third_party/install/lib/libspirv-cross-core.a \
    -o "$probe_work/varying_limits_cpu"
"$probe_work/varying_limits_cpu" "$probe_work"
