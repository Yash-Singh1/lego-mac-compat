#include "crash_trace.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Prepare image metadata under dyld's normal callback, not while crashing.
   Records are append-only: unloading an image must not erase the metadata
   needed to explain an outstanding return address. Publish only complete
   records; no handler-side loader lookup, allocation, or mutex is needed. */
enum { MAX_IMAGES = 2048, MAX_FRAMES = 64, RAW_WORDS = 128 };
static struct { char line[1536]; size_t length; } images[MAX_IMAGES];
static _Atomic unsigned image_count;
static char report_path[1024];
static mach_port_t task;

static void image_added(const struct mach_header *header, intptr_t slide)
{
    unsigned index = atomic_load_explicit(&image_count, memory_order_relaxed);
    if (index == MAX_IMAGES) return;
    if (header->magic != MH_MAGIC_64) return;
    const struct mach_header_64 *mh = (const void *)header;
    const struct load_command *lc = (const void *)(mh + 1);
    unsigned char uuid[16] = {0};
    uint64_t text_end = (uintptr_t)header;
    for (unsigned i = 0; i < mh->ncmds; ++i) {
        if (lc->cmd == LC_UUID) memcpy(uuid, ((const struct uuid_command *)lc)->uuid, 16);
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const void *)lc;
            if (!strcmp(seg->segname, "__TEXT")) text_end = seg->vmaddr + slide + seg->vmsize;
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    char uuid_text[33];
    for (unsigned i = 0; i < 16; ++i) snprintf(uuid_text + i * 2, 3, "%02x", uuid[i]);
    Dl_info info = {0};
    dladdr(header, &info);
    int n = snprintf(images[index].line, sizeof(images[index].line),
        "compat32: crash-image base=0x%llx end=0x%llx uuid=%s path=%s\n",
        (unsigned long long)(uintptr_t)header, (unsigned long long)text_end,
        uuid_text, info.dli_fname ?: "unknown");
    images[index].length = n < 0 ? 0 :
        (size_t)n < sizeof(images[index].line) ? (size_t)n : sizeof(images[index].line) - 1;
    atomic_store_explicit(&image_count, index + 1, memory_order_release);
}

void crash_trace_init(void)
{
    static int initialized;
    if (initialized) return;
    initialized = 1;
    task = mach_task_self();
    _dyld_register_func_for_add_image(image_added);
}

void crash_trace_set_path(const char *path)
{
    strlcpy(report_path, path ?: "", sizeof(report_path));
}

static void write_all(int fd, const char *bytes, size_t length)
{
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        bytes += n; length -= (size_t)n;
    }
}

struct line { char bytes[256]; size_t used; };
static void text(struct line *line, const char *s)
{
    while (*s && line->used < sizeof(line->bytes)) line->bytes[line->used++] = *s++;
}
static void hex(struct line *line, uint64_t value)
{
    text(line, "0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        if (line->used < sizeof(line->bytes)) line->bytes[line->used++] = "0123456789abcdef"[(value >> shift) & 15];
}
static void emit(int fd, struct line *line)
{
    text(line, "\n");
    write_all(fd, line->bytes, line->used);
    if (fd != STDERR_FILENO) write_all(STDERR_FILENO, line->bytes, line->used);
}
static int read_memory(uint64_t address, void *out, size_t size)
{
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(task, address, size, (mach_vm_address_t)out, &copied) == KERN_SUCCESS && copied == size;
}

void crash_trace_capture(int signal_number, uint64_t pc, uint64_t sp,
                         uint64_t fp, uint64_t cs)
{
    if (!task) return;
    /* No backtrace()/dladdr()/stdio here: those can lock the very allocator
       that crashed. Bounded kernel reads fail cleanly on damaged pointers.
       Precompute the path so this report survives last-run.log truncation. */
    int fd = report_path[0] ? open(report_path, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
    if (fd < 0) fd = STDERR_FILENO;
    struct line line = {{0},0};
    text(&line, "compat32: crash-trace-v1 signal="); hex(&line, (unsigned)signal_number);
    text(&line, " pc="); hex(&line, pc); text(&line, " sp="); hex(&line, sp);
    text(&line, " fp="); hex(&line, fp); text(&line, " cs="); hex(&line, cs); emit(fd, &line);
    const int guest = (cs & 0xffff) == 0x23;
    const unsigned width = guest ? 4 : 8;
    if (guest) { pc = (uint32_t)pc; sp = (uint32_t)sp; fp = (uint32_t)fp; }
    uint64_t ceiling = sp <= UINT64_MAX - 16 * 1024 * 1024 ? sp + 16 * 1024 * 1024 : UINT64_MAX;
    const char *stop = "frame-limit";
    for (unsigned i = 0; i < MAX_FRAMES; ++i) {
        line.used = 0;
        text(&line, guest ? "compat32: crash-frame guest index=" : "compat32: crash-frame native index=");
        hex(&line, i); text(&line, " pc="); hex(&line, pc); text(&line, " fp="); hex(&line, fp); emit(fd, &line);
        if (!fp) { stop = "end"; break; }
        if (fp < sp || fp > ceiling - 2 * width || fp % width) { stop = "invalid-frame-pointer"; break; }
        uint64_t pair[2] = {0};
        if (!read_memory(fp, pair, 2 * width)) { stop = "unreadable-frame"; break; }
        uint64_t next = guest ? (uint32_t)pair[0] : pair[0];
        uint64_t ret = guest ? pair[0] >> 32 : pair[1];
        if (!ret) { stop = "end"; break; }
        pc = ret;
        if (next && next <= fp) { stop = "nonascending-frame"; break; }
        fp = next;
    }
    line.used = 0; text(&line, "compat32: crash-unwind-stop "); text(&line, stop); emit(fd, &line);
    /* Raw words are fallback evidence, NOT an asserted call stack. Useful
       where a library omits frame pointers or crosses the guest gateway. */
    for (unsigned i = 0; i < RAW_WORDS && sp <= UINT64_MAX - (i + 1) * width; ++i) {
        uint64_t value = 0;
        if (!read_memory(sp + i * width, &value, width)) break;
        line.used = 0; text(&line, "compat32: crash-stack-word address="); hex(&line, sp + i * width);
        text(&line, " value="); hex(&line, value); emit(fd, &line);
    }
    unsigned count = atomic_load_explicit(&image_count, memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) write_all(fd, images[i].line, images[i].length);
    line.used = 0; text(&line, "compat32: crash-trace-complete"); emit(fd, &line);
    if (fd != STDERR_FILENO) { fsync(fd); close(fd); }
}
