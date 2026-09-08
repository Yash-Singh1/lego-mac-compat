#!/usr/bin/env python3
"""Exercise packed Steam returns and pointer conversion through real i386 calls."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--loader", type=Path, default=native / "build/game_loader")
parser.add_argument("--expect-input-trap", action="store_true")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="lp32-steam-records-") as directory:
    root = Path(directory)
    (root / "bin").mkdir()
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _getenv, _dlopen, _dlsym, _strcmp, _memcmp, _memcpy, _memset, _calloc, _free,
      _crc32, _adler32, _inflateInit_, _inflate, _inflateReset, _inflateEnd,
      _deflateInit2_, _deflate, _deflateReset, _deflateEnd,
      ___stack_chk_guard, ___stack_chk_fail, dyld_stub_binder ]
...
""")
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")

    def build(*command):
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr

    build("xcrun", "clang", "-arch", "x86_64", "-O2", "-dynamiclib",
          str(native / "tests/fixtures/steam_records_host.c"), "-o", str(root / "steamclient.dylib"))
    flags = ["xcrun", "clang", "-target", "i386-apple-macos10.6", "-nostdlib",
             "-fno-builtin", "-L" + str(root), "-lSystem", str(root / "helper.S")]
    build(*flags, "-Wl,-e,_start", str(root / "start.S"), "-o", str(root / "portal2_osx"))
    for optimization in ("-O0", "-O2"):
        build(*flags, optimization, "-dynamiclib", "-Wl,-install_name,@loader_path/launcher.dylib",
              str(native / "tests/fixtures/steam_records_guest.c"),
              str(native / "tests/fixtures/zlib_guest.c"), "-o", str(root / "bin/launcher.dylib"))
        environment = dict(os.environ, LP32_GAME="portal2", LP32_DYLD_SELFTEST="1",
            LP32_DYLD_FIXTURE_SELFTEST="1", LP32_LOG_DIR=str(root / "logs"),
            LP32_STEAM_FIXTURE=str(root / "steamclient.dylib"))
        result = subprocess.run(["arch", "-x86_64", str(args.loader.resolve()), str(root / "portal2_osx")],
            env=environment, capture_output=True, text=True, timeout=60)
        output = result.stdout + result.stderr
        if args.expect_input_trap:
            assert result.returncode != 0 and "trapped import" in output and "_lp32_steam_method_517" in output, output
        else:
            assert result.returncode == 0 and "guest-dyld self-test: PASS" in output, f"exit={result.returncode}\n" + output
        print(f"Steam records {optimization}: {'reproduced old controller trap' if args.expect_input_trap else 'PASS'}")
