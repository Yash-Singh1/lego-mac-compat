#!/usr/bin/env python3
"""Build tiny original i386 fixtures with Apple's linker; no game/old SDK needed."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]
LOADER = NATIVE / "build/game_loader"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--math-only", action="store_true",
                    help="Exercise math imports without requiring active displays or Carbon UI")
options = parser.parse_args()


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


with tempfile.TemporaryDirectory(prefix="lp32-dyld-") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "carbon-test.txt").write_bytes(b"fixture\n")
    # Link-time declarations only. All actual system calls go through the bridge.
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [
      _opendir$INODE64, _readdir$INODE64, _readdir_r, _closedir, _closedir$UNIX2003, _setlocale, _vswprintf, _swprintf, _wcscmp, _strlen,
      _getrusage, _asinf, _atanf, _finite, ___fixunssfdi, _GetGlobalMouse,
      _cbrt, _coshf, _sinhf, _tanhf, _frexpf, _ldexpf, _modff,
      _CGDisplayHideCursor, _CGDisplayShowCursor, _CGCursorIsVisible,
      _CFStringGetBytes, _CFCharacterSetGetPredefined, _CFCharacterSetIsLongCharacterMember, _UpTime, _AbsoluteToNanoseconds, _NanosecondsToAbsolute,
      ___divdi3, ___moddi3, _setjmp, __setjmp, _sigsetjmp, _longjmp,
      __longjmp, _siglongjmp, _sigprocmask, _sigprocmask$UNIX2003, _memcpy, ___memset_chk,
      _pthread_create_suspended_np, _pthread_mach_thread_np, _thread_resume, _pthread_join, _pthread_join$UNIX2003, _usleep,
      _usleep$UNIX2003, _AudioQueueNewOutput, _AudioQueueAddPropertyListener, _AudioQueueAllocateBuffer, _AudioQueueSetParameter, _AudioQueueGetParameter,
      _AudioQueueEnqueueBuffer, _AudioQueueStart, _AudioQueueStop, _AudioQueueRemovePropertyListener, _AudioQueueFreeBuffer, _AudioQueueDispose,
      _strcmp, _lp32_unavailable, _stat, _stat$INODE64, _open$UNIX2003, _open,
      _close, _close$UNIX2003, _lseek, _scandir, _alphasort, _free,
      _sigaction, _raise, _getpid, _iconv_open, _iconv, _iconv_close,
      _mmap, _mmap$UNIX2003, _munmap, _munmap$UNIX2003, _mprotect, _mprotect$UNIX2003,
      _getpagesize, _memset, _dlopen, _dlsym, _dlerror, _dlclose,
      _CFRunLoopGetCurrent, _CFRunLoopSourceCreate, _CFRunLoopAddSource, _CFRunLoopContainsSource, _CFRunLoopSourceSignal, _CFRunLoopRunInMode,
      _CFRunLoopRemoveSource, _CFRunLoopTimerCreate, _CFRunLoopAddTimer, _CFRunLoopRemoveTimer, _CFRunLoopStop, _CFRelease,
      _kCFRunLoopDefaultMode, _CGMainDisplayID, _CGDisplayBounds, _mktime, _localtime_r, _timegm,
      _gethostname, _gethostbyname, _socket, _bind, _bind$UNIX2003, _getsockname,
      _setsockopt, _getsockopt, _sendto, _sendto$UNIX2003, _recvfrom, _recvfrom$UNIX2003,
      _select, _select$UNIX2003, _select$DARWIN_EXTSN, _ioctl, _popen, _popen$UNIX2003,
      _pclose, _valloc, _realloc, _malloc_size, _MPCreateCriticalRegion, _MPDeleteCriticalRegion,
      _MPEnterCriticalRegion, _MPExitCriticalRegion, _NSIsSymbolNameDefined, _NSLookupAndBindSymbol, _NSAddressOfSymbol, _NSModuleForSymbol,
      _NSLibraryNameForModule, _fgets, _fgets$UNIX2003, ___darwin_check_fd_set_overflow, _getsockname$UNIX2003, _mktime$UNIX2003,
      _CFStringCreateWithBytes, _CFStringGetLength, _CFRetain,
      _CFStringCreateWithCString, _ATSFontFindFromName, _ATSUCreateStyle, _ATSUSetAttributes, _ATSUCreateTextLayoutWithTextPtr, _ATSUSetLayoutControls,
      _ATSUDirectGetLayoutDataArrayPtrFromTextLayout, _ATSUGlyphGetScreenMetrics, _ATSUDrawText, _ATSUDisposeTextLayout, _ATSUDisposeStyle, _CGColorSpaceCreateDeviceRGB,
      _CGColorSpaceRelease, _CGBitmapContextCreate, _CGBitmapContextGetData, _CGContextRelease, ___stack_chk_guard, ___stack_chk_fail,
      ___CFConstantStringClassReference, _memcmp, _FSPathMakeRef, _FSGetCatalogInfo, _PBMakeFSRefSync, _FSCompareFSRefs, _FSOpenFork, _FSReadFork, _PBReadForkAsync, _FSCloseFork,
      _AudioConverterNew, _AudioConverterFillBuffer, _AudioConverterDispose,
      _CGContextSaveGState, _CGContextRestoreGState, _CGContextSetRGBFillColor, _CGContextFillRect, _CGContextClearRect,
      _DMGetDeskRegion, _GetRegionBounds, _NewRgn, _DisposeRgn,
      _getenv, _GetApplicationEventTarget, _InstallEventHandler, _CreateEvent, _SetEventParameter, _GetEventParameter,
      _GetEventClass, _GetEventKind, _GetEventTime, _SendEventToEventTarget, _CallNextEventHandler, _RetainEvent,
      _GetEventRetainCount, _ReleaseEvent, _RemoveEventHandler, _MPAllocateTaskStorageIndex, _MPSetTaskStorageValue,
      _MPGetTaskStorageValue, _MPCreateEvent, _MPSetEvent, _MPWaitForEvent, _MPDeleteEvent, _MPCreateTask,
      _CFStringCreateWithFormat, _CFStringCreateMutableCopy, _CFStringAppend, _CFStringGetCString,
      _PBHGetVInfoSync, _GetCurrentProcess, _GetProcessInformation,
      _HIObjectRegisterSubclass, _HIObjectCreate, _calloc,
      _GetCurrentEventLoop, _RunCurrentEventLoop, _QuitEventLoop, _InstallEventLoopTimer, _RemoveEventLoopTimer,
      _GetMainDevice, _GetDeviceList, _GetNextDevice, _DMGetDisplayIDByGDevice, _DMGetGDeviceByDisplayID, _TestDeviceAttribute,
      _FSGetVolumeInfo,
      _GetCurrentEventQueue, _PostEventToQueue, _ReceiveNextEvent, _FSOpenIterator, _FSGetCatalogInfoBulk, _FSCloseIterator,
      _XML_ParserCreate_MM, _XML_ParserFree, _XML_Parse, _XML_SetUserData, _XML_SetElementHandler, _XML_SetCharacterDataHandler,
      _XML_GetErrorCode, _XML_GetCurrentLineNumber, _XML_ErrorString, _malloc,
      dyld_stub_binder
    ]
...
""")
    (root / "dep.c").write_text("""
int value = 11;
int *value_pointer = &value;
__attribute__((constructor)) static void initialize(void) { *value_pointer *= 2; }
int dependency_value(void) { return *value_pointer; }
""")
    (root / "launcher.c").write_text("""
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <iconv.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc/malloc.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
extern int dependency_value(void);
extern int strcmp(const char *, const char *) __attribute__((weak_import));
extern int lp32_unavailable(void) __attribute__((weak_import));
extern int check_directory(void);
extern int check_carbon(void);
extern int check_font(void);
extern int check_context(void);
extern int check_math_imports(void);
extern int check_audio_threads(void);
extern int MPCreateCriticalRegion(void **);
extern int MPDeleteCriticalRegion(void *);
extern int MPEnterCriticalRegion(void *, int);
extern int MPExitCriticalRegion(void *);
extern int NSIsSymbolNameDefined(const char *);
extern void *NSLookupAndBindSymbol(const char *);
extern void *NSAddressOfSymbol(void *);
extern void *NSModuleForSymbol(void *);
extern const char *NSLibraryNameForModule(void *);
int (*dependency_pointer)(void) = dependency_value;
int initialized, count;
static volatile sig_atomic_t received_signal;
static void source_ready(void *info) { ++*(int *)info; }
static void timer_ready(CFRunLoopTimerRef timer, void *info) {
    ++*(int *)info; CFRunLoopStop(CFRunLoopGetCurrent());
}
static void child_changed(int signal_number) { received_signal = signal_number; }
__attribute__((constructor)) static void initialize(void) {
    initialized = dependency_pointer() + 3;
    ++count;
}
int LauncherMain(void) {
    { int status = check_math_imports(); if (status) return status; }
    if (getenv("LP32_DYLD_MATH_ONLY")) return 26;
    { int status = check_carbon(); if (status) return -1000 + status; }
    if (!strcmp || lp32_unavailable) return -4;
    int context_error = check_context(); if (context_error) return context_error;
    int audio_error = check_audio_threads(); if (audio_error) return audio_error;
    int font_error = check_font(); if (font_error) return font_error;
    void *system = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    if (!system || dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY) != system) return -17;
    int (*compare)(const char *, const char *) = dlsym(system, "strcmp");
    if (!compare || compare("same", "same") || compare("a", "z") >= 0) return -18;
    if (dlsym(system, "lp32_unavailable") || !dlerror() || dlerror()) return -19;
    if (dlclose(system) || compare("still mapped", "still mapped")) return -20;
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return -21;
    int performed = 0, fired = 0;
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopSourceContext context = {0};
    context.info = &performed; context.perform = source_ready;
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(0, 0, &context);
    if (!source) return -22;
    CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    if (!CFRunLoopContainsSource(loop, source, kCFRunLoopDefaultMode)) return -23;
    CFRunLoopSourceSignal(source);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, 1);
    CFRunLoopRemoveSource(loop, source, kCFRunLoopDefaultMode);
    if (performed != 1 || CFRunLoopContainsSource(loop, source, kCFRunLoopDefaultMode)) return -24;
    CFRelease(source);
    CFRunLoopTimerContext timer_context = {0}; timer_context.info = &fired;
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(0, 0, 0, 0, 0, timer_ready, &timer_context);
    if (!timer) return -25;
    CFRunLoopAddTimer(loop, timer, kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, 0);
    CFRunLoopRemoveTimer(loop, timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
    if (fired != 1) return -26;
    struct { unsigned before; struct tm value; unsigned after; } calendar = {0};
    calendar.before = 0x12345678; calendar.after = 0x87654321;
    calendar.value.tm_year = 120; calendar.value.tm_mon = 1;
    calendar.value.tm_mday = 30; calendar.value.tm_hour = 12; calendar.value.tm_isdst = -1;
    time_t timestamp = mktime(&calendar.value);
    struct tm decoded;
    if (timestamp == (time_t)-1 || !localtime_r(&timestamp, &decoded) ||
        decoded.tm_mon != 2 || decoded.tm_mday != 1 || decoded.tm_hour != 12 ||
        calendar.value.tm_mon != 2 || calendar.value.tm_mday != 1 ||
        calendar.before != 0x12345678 || calendar.after != 0x87654321) return -27;
    calendar.value.tm_year = 200;
    if (timegm(&calendar.value) != (time_t)-1) return -28;
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) || !hostname[0]) return -29;
    struct hostent *host = gethostbyname("localhost");
    if (!host || host->h_addrtype != AF_INET || host->h_length != 4 ||
        !host->h_name[0] || !host->h_addr_list[0] || ((unsigned char *)host->h_addr_list[0])[0] != 127) return -30;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0 || socket_fd >= FD_SETSIZE) return -31;
    struct sockaddr_in endpoint = {0};
    endpoint.sin_len = sizeof(endpoint); endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t endpoint_size = sizeof(endpoint);
    struct timeval timeout = {0, 200000}, recovered = {0};
    socklen_t timeout_size = sizeof(recovered);
    if (bind(socket_fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) ||
        getsockname(socket_fd, (struct sockaddr *)&endpoint, &endpoint_size) ||
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        getsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &recovered, &timeout_size) ||
        timeout_size != sizeof(recovered) || recovered.tv_usec != timeout.tv_usec) return -32;
    if (sendto(socket_fd, "udp", 3, 0, (struct sockaddr *)&endpoint, endpoint_size) != 3) return -33;
    fd_set read_set; FD_ZERO(&read_set); FD_SET(socket_fd, &read_set);
    if (select(socket_fd + 1, &read_set, 0, 0, &timeout) != 1 || !FD_ISSET(socket_fd, &read_set)) return -34;
    int available = 0;
    if (ioctl(socket_fd, FIONREAD, &available) || available < 3) return -35;
    char packet[4] = {0};
    if (recvfrom(socket_fd, packet, 3, 0, (struct sockaddr *)&endpoint, &endpoint_size) != 3 ||
        strcmp(packet, "udp") || close(socket_fd)) return -36;
    FILE *command = popen("printf pipe; exit 7", "r");
    char command_output[8] = {0};
    if (!command || fgets(command_output, sizeof(command_output), command) != command_output ||
        strcmp(command_output, "pipe")) return -37;
    int child_status = pclose(command);
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 7) return -38;
    void *region = 0;
    if (MPCreateCriticalRegion(&region) || !region || MPEnterCriticalRegion(region, 0) ||
        MPEnterCriticalRegion(region, 0x7fffffff) || MPExitCriticalRegion(region) ||
        MPExitCriticalRegion(region) || MPDeleteCriticalRegion(region)) return -39;
    char *aligned = valloc(7000);
    if (!aligned || ((unsigned long)aligned & (getpagesize() - 1)) || malloc_size(aligned) < 7000) return -52;
    aligned[6999] = 'v';
    if (mprotect(aligned, 7000, PROT_READ)) return -53;
    char *resized = realloc(aligned, 12000);
    if (!resized || resized[6999] != 'v' || malloc_size(resized) < 12000) return -54;
    free(resized);
    aligned = valloc(32); if (!aligned) return -55;
    free(aligned);
    if (!NSIsSymbolNameDefined("_LauncherMain") || NSIsSymbolNameDefined("_lp32_unavailable")) return -56;
    void *legacy_symbol = NSLookupAndBindSymbol("_LauncherMain");
    void *legacy_module = NSModuleForSymbol(legacy_symbol);
    const char *legacy_path = NSLibraryNameForModule(legacy_module);
    if (NSAddressOfSymbol(legacy_symbol) != (void *)&LauncherMain || !legacy_module ||
        !legacy_path || !legacy_path[0]) return -57;
    if (!check_directory()) return -5;
    struct sigaction action = {0}, old_action;
    action.sa_handler = child_changed;
    received_signal = 0;
    if (sigaction(SIGCHLD, &action, &old_action) || raise(SIGCHLD)) return -6;
    (void)getpid(); /* Pending signal delivery at the next guest bridge call. */
    if (received_signal != SIGCHLD || sigaction(SIGCHLD, &old_action, 0)) return -7;
    iconv_t converter = iconv_open("UTF-16LE", "UTF-8");
    if (converter == (iconv_t)-1) return -8;
    char input[] = "a", output[8] = {0}, *in = input, *out = output;
    size_t in_left = 1, out_left = sizeof(output);
    if (iconv(converter, &in, &in_left, &out, &out_left) == (size_t)-1 ||
        in_left || out_left != 6 || out != output + 2 || output[0] != 'a' || output[1]) return -9;
    if (iconv_close(converter)) return -10;
    struct { unsigned before; struct stat metadata; unsigned after; } result;
    result.before = 0x12345678; result.after = 0x87654321;
    if (stat("probe.txt", &result.metadata) || result.metadata.st_size != 7 ||
        !S_ISREG(result.metadata.st_mode) || result.before != 0x12345678 || result.after != 0x87654321) return -1;
    int fd = open("probe.txt", O_RDONLY);
    if (fd < 0) return -2;
    off_t end = lseek(fd, 0, SEEK_END);
    if (close(fd) || end != 7) return -3;
    fd = open("probe-large", O_RDONLY);
    if (fd < 0) return -11;
    off_t large_offset = ((off_t)5 << 30) + 7;
    off_t position = lseek(fd, large_offset, SEEK_SET);
    end = lseek(fd, 0, SEEK_END);
    char *mapped = mmap(0, 7, PROT_READ, MAP_PRIVATE, fd, (off_t)5 << 30);
    if (close(fd) || position != large_offset || end != large_offset) return -12;
    if (mapped == MAP_FAILED || mapped[6] || mprotect(mapped, 7, PROT_READ | PROT_WRITE)) return -13;
    mapped[6] = 'x';
    if (munmap(mapped, 7)) return -14;
    size_t page = getpagesize();
    mapped = mmap(0, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED || mmap(mapped + page, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != mapped + page) return -15;
    mapped[page] = 'y';
    if (munmap(mapped, page) || munmap(mapped + page * 2, page) || mapped[page] != 'y' ||
        munmap(mapped + page, page)) return -16;
    return initialized + count + strcmp("same", "same");
}
""")
    (root / "probe.txt").write_text("portal2")
    with (root / "probe-large").open("wb") as sparse:
        sparse.truncate((5 << 30) + 7)
    (root / "probe-dir").mkdir()
    for name in ("zeta", "alpha", "ignored"):
        (root / "probe-dir" / name).touch()
    (root / "directory.c").write_text("""
#define _DARWIN_NO_64_BIT_INODE
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
struct __attribute__((packed, aligned(4))) dirent64_fixture {
    uint64_t inode, seekoff;
    uint16_t reclen, namlen;
    uint8_t type;
    char name[1024];
};
extern DIR *open64_directory(const char *) __asm("_opendir$INODE64");
extern struct dirent64_fixture *read64_directory(DIR *) __asm("_readdir$INODE64");
static int select_entry(const struct dirent *entry) {
    return entry->d_name[0] == 'a' || entry->d_name[0] == 'z';
}
int check_directory(void) {
    struct dirent **entries = 0;
    int count = scandir("probe-dir", &entries, select_entry, alphasort);
    int ok = count == 2 && entries[0]->d_name[0] == 'a' && entries[1]->d_name[0] == 'z';
    for (int i = 0; i < count; ++i) free(entries[i]);
    free(entries);
    DIR *directory = open64_directory("probe-dir");
    if (!directory) return 0;
    struct { struct dirent entry; uint32_t guard; } legacy = {{0}, 0xabcd1234};
    struct dirent *legacy_result = NULL;
    if (readdir_r(directory, &legacy.entry, &legacy_result) || legacy_result != &legacy.entry ||
        legacy.guard != 0xabcd1234 || !legacy.entry.d_namlen || !legacy.entry.d_name[0]) ok = 0;
    closedir(directory);
    directory = open64_directory("probe-dir");
    if (!directory) return 0;
    unsigned found = 0;
    struct dirent64_fixture *entry;
    while ((entry = read64_directory(directory))) {
        if (!strcmp(entry->name, "alpha") || !strcmp(entry->name, "zeta") || !strcmp(entry->name, "ignored")) {
            if (!entry->inode || entry->type != DT_REG || entry->namlen != strlen(entry->name) ||
                entry->reclen < 21 + entry->namlen + 1) ok = 0;
            ++found;
        }
    }
    return !closedir(directory) && found == 3 && ok;
}
""")
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    for version, address in (("10.5", 0), ("10.6", 0),
                             ("10.5", 0x90000000), ("10.6", 0x90000000)):
        flags = ["xcrun", "clang", "-target", "i386-apple-macos" + version,
                 "-nostdlib", "-fno-builtin", "-L" + str(root), "-lSystem", str(root / "helper.S")]
        run("xcrun", "clang", "-target", "i386-apple-macos" + version,
            "-c", str(root / "directory.c"), "-o", str(root / "directory.o"))
        run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/libfixture_dep.dylib",
            f"-Wl,-seg1addr,0x{address:x}",
            str(root / "dep.c"), "-o", str(root / "bin/libfixture_dep.dylib"))
        dependency = "fixture_dep"
        if version == "10.6":
            (root / "exports.txt").write_text("_dependency_value\n")
            run(*flags, "-target", "i386-apple-macos10.7", "-dynamiclib", "-Wl,-install_name,@loader_path/libfixture_reexport.dylib",
                "-Wl,-reexported_symbols_list," + str(root / "exports.txt"),
                "-L" + str(root / "bin"), "-lfixture_dep", "-o", str(root / "bin/libfixture_reexport.dylib"))
            dependency = "fixture_reexport"
        run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/launcher.dylib",
            str(root / "launcher.c"), str(NATIVE / "tests/fixtures/font.c"), str(NATIVE / "tests/fixtures/carbon.c"), str(NATIVE / "tests/fixtures/context.c"), str(NATIVE / "tests/fixtures/audio_threads.c"), str(root / "directory.o"), "-L" + str(root / "bin"), "-l" + dependency,
            "-o", str(root / "bin/launcher.dylib"))
        run(*flags, "-Wl,-e,_start,-no_pie", str(root / "start.S"), "-o", str(root / "portal2_osx"))
        # Exercise universal i386 selection on a real generated dylib.
        dep = root / "bin/libfixture_dep.dylib"
        thin = dep.read_bytes()
        cursor = 28
        commands = []
        for _ in range(struct.unpack_from("<I", thin, 16)[0]):
            command, size = struct.unpack_from("<II", thin, cursor)
            commands.append(command)
            cursor += size
        compressed = 0x22 in commands or 0x80000022 in commands
        assert compressed == (version == "10.6"), commands
        fat = struct.pack(">7I", 0xCAFEBABE, 1, 7, 3, 4096, len(thin), 12)
        dep.write_bytes(fat.ljust(4096, b"\0") + thin)
        # Force failure after read_module allocates its file, before any execution.
        malformed = bytearray(thin)
        struct.pack_into("<I", malformed, 20, 0xFFFFFFFF)
        (root / "bin/broken.dylib").write_bytes(malformed)
        environment = dict(os.environ, LP32_CARBON_FIXTURE_FILE=str(root / "carbon-test.txt"), LP32_GAME="portal2", LP32_DYLD_SELFTEST="1",
                           LP32_DYLD_FIXTURE_SELFTEST="1")
        environment.pop("LP32_DYLD_MATH_ONLY", None)
        if options.math_only:
            environment["LP32_DYLD_MATH_ONLY"] = "1"
        run("arch", "-x86_64", str(LOADER), str(root / "portal2_osx"), env=environment)
        print(f"Guest dylib fixtures{' (math only)' if options.math_only else ''}: PASS (macOS {version}, preferred address 0x{address:x})")
