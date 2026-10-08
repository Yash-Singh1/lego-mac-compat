# Sourced by piglit.sh and glcts.sh (needs $here and $out): builds
# headless_shim.m and checks that test executables link it. The shim is
# linked into every executable rather than inserted with
# DYLD_INSERT_LIBRARIES, which SIP-protected launchers (/bin/sh, /usr/bin/env
# children of them) silently drop, letting test windows show and take focus.
shim=$out/libheadless_shim.dylib

build_shim() {
    mkdir -p "$out"
    if [ ! -f "$shim" ] || [ "$here/headless_shim.m" -nt "$shim" ]; then
        xcrun clang -arch arm64 -arch x86_64 -dynamiclib -fobjc-arc -Wno-deprecated-declarations \
            -install_name "$shim" "$here/headless_shim.m" -framework AppKit -framework Carbon -o "$shim"
    fi
}

# Linker flags that make an executable load the shim.
shim_link_flags() { printf '%s' "-Wl,-needed_library,$shim"; }

# require_headless EXECUTABLE...: refuse to run tests that could show windows.
require_headless() {
    for exe in "$@"; do
        case $(file -b "$exe") in Mach-O*) ;; *) continue ;; esac
        if ! otool -L "$exe" 2>/dev/null | grep -qF "$shim"; then
            echo "refusing to run: $exe does not link $shim (its windows would show); rebuild with '$0 build'" >&2
            exit 1
        fi
    done
}
