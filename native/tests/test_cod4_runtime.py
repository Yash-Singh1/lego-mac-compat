#!/usr/bin/env python3
"""Exercise guest dylib relocation, constructors, context jumps and libc ABI.

Builds original tiny i386 fixtures with the current Apple toolchain. No game,
Steam client, old SDK, or private C++ libraries are needed.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]

def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)

with tempfile.TemporaryDirectory(prefix="lp32-cod4-fixture-") as temporary:
    root = Path(temporary)
    (root / "bin").mkdir()
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _setjmp, _longjmp, _sigsetjmp, _siglongjmp,
      _sigprocmask, _sigprocmask$UNIX2003, _lround, _lroundf, _asinf,
      _acosf, _atof, _getrlimit, _getrlimit$UNIX2003, _setrlimit,
      _setrlimit$UNIX2003, _getcwd, _free, _memset, _memcpy,
      _dlopen, _dlsym, _dlclose, _dlerror, ___maskrune, ___tolower,
      _strlen, _strcmp, _memcmp, _strcpy, _strncpy, _pthread_self,
      _pthread_mutex_lock, _pthread_mutex_unlock, _pthread_mutex_destroy,
      _pthread_cond_destroy, _pthread_cond_timedwait, _pthread_cond_timedwait$UNIX2003,
      _pthread_sigmask, _pthread_sigmask$UNIX2003,
      _pthread_setcanceltype, _pthread_setcanceltype$UNIX2003, _vsscanf, _getenv, ___error,
      _nanosleep, _nanosleep$UNIX2003,
      _iconv_open, _iconv_close, _iconv, _iconvctl,
      _wcslen, _wcscpy, _wcscat, _wcschr, _wcsrchr, _wcsstr, _wmemcmp,
      _host_page_size, _mach_host_self, _mach_task_self_, _mach_vm_region, _vm_page_size,
      _mmap, _mmap$UNIX2003, _munmap, _munmap$UNIX2003, _mprotect, _mprotect$UNIX2003,
      _OSAtomicCompareAndSwap64Barrier,
      __ZNSt3__19to_stringEm,
      __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev,
      __ZNSt3__16chrono12system_clock3nowEv, __ZNSt3__16chrono12steady_clock3nowEv,
      __ZNSt3__118condition_variable10notify_allEv, __ZNSt3__118condition_variable10notify_oneEv,
      __ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILx1ELx1000000000EEEEEEE,
      __ZNSt3__16futureIvED1Ev, __ZNSt3__16futureIvE3getEv,
      __ZNSt3__17promiseIvEC1Ev, __ZNSt3__17promiseIvED1Ev,
      __ZNSt3__17promiseIvE10get_futureEv, __ZNSt3__17promiseIvE9set_valueEv,
      _pthread_create, _pthread_join, _pthread_join$UNIX2003,
      _pthread_key_create, _pthread_getspecific, _pthread_setspecific, _setlocale, dyld_stub_binder ]
...
""")
    (root / "dep.c").write_text("""
int value = 11;
int *value_pointer = &value;
__attribute__((constructor)) static void initialize(void) { *value_pointer *= 2; }
int dependency_value(void) { return *value_pointer; }
""")
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    for version, address in (("10.5", 0), ("10.6", 0), ("10.5", 0x90000000), ("10.6", 0x90000000)):
        flags = ["xcrun", "clang", "-target", "i386-apple-macos" + version,
                 "-nostdlib", "-fno-builtin", "-fno-stack-protector", "-D_FORTIFY_SOURCE=0",
                 "-L" + str(root), "-lSystem", str(root / "helper.S")]
        run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/libfixture_dep.dylib",
            f"-Wl,-seg1addr,0x{address:x}", str(root / "dep.c"), "-o", str(root / "bin/libfixture_dep.dylib"))
        run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/fixture.dylib",
            str(NATIVE / "tests/fixtures/cod4_runtime.c"),
            str(NATIVE / "tests/fixtures/mw2_future.cpp"), "-fno-exceptions", "-fno-rtti",
            "-L" + str(root / "bin"), "-lfixture_dep",
            "-o", str(root / "bin/fixture.dylib"))
        run(*flags, "-Wl,-e,_start,-no_pie", str(root / "start.S"), "-o", str(root / "COD4.image"))
        dep = root / "bin/libfixture_dep.dylib"
        thin = dep.read_bytes()
        cursor, commands = 28, []
        for _ in range(struct.unpack_from("<I", thin, 16)[0]):
            command, size = struct.unpack_from("<II", thin, cursor)
            commands.append(command)
            cursor += size
        assert (0x22 in commands or 0x80000022 in commands) == (version == "10.6")
        fat = struct.pack(">7I", 0xCAFEBABE, 1, 7, 3, 4096, len(thin), 12)
        dep.write_bytes(fat.ljust(4096, b"\0") + thin)
        malformed = bytearray(thin)
        struct.pack_into("<I", malformed, 20, 0xFFFFFFFF)
        (root / "bin/broken.dylib").write_bytes(malformed)
        env = dict(os.environ, LP32_GAME="cod4", LP32_DYLD_FIXTURE_SELFTEST="1", LP32_NO_DIAGNOSTIC_LOG="1")
        for profile in ("cod4", "mw2"):
            env['LP32_GAME'] = profile
            run("arch", "-x86_64", str(NATIVE / "build/game_loader"), str(root / "COD4.image"), env=env, timeout=30)
        print(f"COD4 runtime fixture PASS (macOS {version}, preferred base {address:#x})")
