#include "crash_trace.h"
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>

/* Keep physics observations independently: a busy allocator must not evict
   the evidence of the last simulation tick. Static native memory, not guest
   heap memory which the fault under investigation may have corrupted. */
enum { HEAP_EVENTS = 65536, PHYSICS_EVENTS = 4096 };
struct event {
    _Atomic unsigned busy;
    _Atomic uint64_t sequence, time;
    _Atomic uint32_t kind, address, size, site, detail;
};
static struct event heap_events[HEAP_EVENTS], physics_events[PHYSICS_EVENTS];
static _Atomic uint64_t sequence, heap_cursor, physics_cursor, dropped;
static struct { char path[4096], uuid[33]; uint32_t base, end, slide; } images[129];
static _Atomic unsigned image_count;

/* Called under the dyld loader lock. Entries remain immutable after publish. */
void lp32_trace_module(const char *path, uint32_t base, uint32_t end,
                       uint32_t slide, const unsigned char uuid[16])
{
    unsigned n = atomic_load_explicit(&image_count, memory_order_relaxed);
    if (n == 129) return;
    unsigned i = 0;
    while (path[i] && i+1 < sizeof(images[n].path)) { images[n].path[i] = path[i]; ++i; }
    images[n].path[i] = 0;
    for (i = 0; i < 16; ++i) {
        images[n].uuid[i*2] = "0123456789abcdef"[uuid ? uuid[i] >> 4 : 0];
        images[n].uuid[i*2+1] = "0123456789abcdef"[uuid ? uuid[i] & 15 : 0];
    }
    images[n].base=base; images[n].end=end; images[n].slide=slide;
    atomic_store_explicit(&image_count, n+1, memory_order_release);
}
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "crash recorder atomics must not take locks");

void lp32_trace_record(unsigned kind, uint32_t address, uint32_t size,
                       uint32_t site, uint32_t detail)
{
    int physics = kind >= LP32_TRACE_ANIM_BEFORE;
    uint64_t index = atomic_fetch_add_explicit(physics ? &physics_cursor : &heap_cursor,
                                               1, memory_order_relaxed);
    struct event *e = physics ? &physics_events[index % PHYSICS_EVENTS] :
                               &heap_events[index % HEAP_EVENTS];
    if (atomic_exchange_explicit(&e->busy, 1, memory_order_acquire)) {
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        return;
    }
    /* Atomic payload fields make concurrent crash reads defined, even if a
       writer wraps the ring while it is being captured. Zero means partial. */
    atomic_store_explicit(&e->sequence, 0, memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->kind, kind, memory_order_relaxed);
    atomic_store_explicit(&e->address, address, memory_order_relaxed);
    atomic_store_explicit(&e->size, size, memory_order_relaxed);
    atomic_store_explicit(&e->site, site, memory_order_relaxed);
    atomic_store_explicit(&e->detail, detail, memory_order_relaxed);
    atomic_store_explicit(&e->time, mach_absolute_time(), memory_order_relaxed);
    atomic_store_explicit(&e->sequence,
        atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed) + 1,
        memory_order_release);
    atomic_store_explicit(&e->busy, 0, memory_order_release);
}

int lp32_crash_read(uint64_t address, void *out, size_t size)
{
    if (!address || address + size < address) return 0;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
        (mach_vm_address_t)(uintptr_t)out, &copied) == KERN_SUCCESS && copied == size;
}

int lp32_crash_thread_stack(void)
{
    static _Thread_local unsigned char memory[128*1024] __attribute__((aligned(16)));
    stack_t existing;
    if (sigaltstack(NULL, &existing)) return 0;
    if (!(existing.ss_flags & SS_DISABLE)) return 1;
    stack_t stack = {.ss_sp=memory, .ss_size=sizeof(memory)};
    return sigaltstack(&stack, NULL) == 0;
}

/* No stdio, malloc, mutexes, symbol lookup or guest dereferences in this
   writer. A corrupt stack or PROT_NONE object must not crash the reporter. */
struct writer { int fd; unsigned used; char data[4096]; };
static void flush(struct writer *w)
{
    unsigned done = 0;
    while (done < w->used) {
        ssize_t n = write(w->fd, w->data + done, w->used - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (unsigned)n;
    }
    w->used = 0;
}
static void string(struct writer *w, const char *s)
{
    while (*s) {
        if (w->used == sizeof(w->data)) flush(w);
        w->data[w->used++] = *s++;
    }
}
static void hex(struct writer *w, uint64_t n)
{
    char s[19] = "0x0000000000000000";
    for (unsigned i = 0; i < 16; ++i) s[17-i] = "0123456789abcdef"[(n >> (i*4)) & 15];
    string(w, s);
}
static void field(struct writer *w, const char *name, uint64_t n)
{ string(w, name); hex(w, n); }

static void words(struct writer *w, const char *label, uint64_t address, unsigned count)
{
    if (count > 1024) count = 1024;
    for (unsigned i = 0; i < count; i += 4) {
        uint32_t data[4];
        unsigned n = count-i < 4 ? count-i : 4;
        string(w, "capture memory "); string(w, label);
        field(w, " address=", address + i*4);
        if (!lp32_crash_read(address + i*4, data, n*4)) {
            string(w, " unreadable\n"); break;
        }
        for (unsigned j = 0; j < n; ++j) field(w, " ", data[j]);
        string(w, "\n");
    }
}
void lp32_crash_dump_words(int fd, const char *label, uint64_t address, unsigned count)
{ struct writer w = {.fd=fd}; words(&w, label, address, count); flush(&w); }

static void events(struct writer *w, struct event *ring, unsigned capacity, uint64_t end)
{
    uint64_t start = end > capacity ? end-capacity : 0;
    for (uint64_t i = start; i < end; ++i) {
        struct event *e = &ring[i % capacity];
        if (atomic_load_explicit(&e->busy, memory_order_acquire)) continue;
        uint64_t seq = atomic_load_explicit(&e->sequence, memory_order_acquire);
        if (!seq) continue;
        uint64_t time = atomic_load_explicit(&e->time, memory_order_relaxed);
        uint32_t kind = atomic_load_explicit(&e->kind, memory_order_relaxed);
        uint32_t addr = atomic_load_explicit(&e->address, memory_order_relaxed);
        uint32_t size = atomic_load_explicit(&e->size, memory_order_relaxed);
        uint32_t site = atomic_load_explicit(&e->site, memory_order_relaxed);
        uint32_t detail = atomic_load_explicit(&e->detail, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&e->sequence, memory_order_acquire) != seq ||
            atomic_load_explicit(&e->busy, memory_order_acquire)) continue;
        field(w, "capture event seq=", seq); field(w, " time=", time);
        field(w, " kind=", kind); field(w, " address=", addr);
        field(w, " size=", size); field(w, " site=", site);
        field(w, " detail=", detail); string(w, "\n");
    }
}

void lp32_crash_capture(int fd, uint64_t pc, uint64_t sp, uint64_t bp,
                       const uint64_t *registers, size_t count, int guest)
{
    struct writer w = {.fd=fd};
    string(&w, "capture begin version=1\n");
    unsigned image_total = atomic_load_explicit(&image_count, memory_order_acquire);
    for (unsigned i = 0; i < image_total; ++i) {
        field(&w, "capture image base=", images[i].base);
        field(&w, " end=", images[i].end); field(&w, " slide=", images[i].slide);
        string(&w, " uuid="); string(&w, images[i].uuid);
        string(&w, " path="); string(&w, images[i].path); string(&w, "\n");
    }
    field(&w, "capture pc=", pc); field(&w, " sp=", sp); field(&w, " bp=", bp);
    field(&w, " heap-total=", atomic_load(&heap_cursor));
    field(&w, " physics-total=", atomic_load(&physics_cursor));
    field(&w, " dropped=", atomic_load(&dropped)); string(&w, "\n");
    for (size_t i = 0; i < count; ++i) {
        field(&w, "capture register index=", i); field(&w, " value=", registers[i]);
        string(&w, "\n");
    }
    words(&w, "instruction", pc > 16 ? pc-16 : pc, 24);
    words(&w, "stack", sp, 1024);
    uint64_t frame = bp;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t next, ret;
        if (guest) {
            uint32_t data[2];
            if (frame > UINT32_MAX || !lp32_crash_read(frame, data, sizeof(data))) break;
            next = data[0]; ret = data[1];
        } else {
            uint64_t data[2];
            if (!lp32_crash_read(frame, data, sizeof(data))) break;
            next = data[0]; ret = data[1];
        }
        field(&w, "capture frame address=", frame); field(&w, " return=", ret);
        string(&w, "\n");
        if (next <= frame || next-frame > 1024*1024) break;
        frame = next;
    }
    /* Guest object + preceding allocator header, and one level of pointers.
       Candidate stack words include the this argument at a failed call. */
    uint64_t candidates[32]; unsigned used = 0;
    for (size_t i = 0; i < count && used < 16; ++i) candidates[used++] = registers[i];
    if (guest) {
        uint32_t stack[16];
        if (lp32_crash_read(sp, stack, sizeof(stack)))
            for (unsigned i = 0; i < 16 && used < 32; ++i) candidates[used++] = stack[i];
    }
    for (unsigned i = 0; i < used; ++i) {
        uint64_t addr = candidates[i];
        if (addr < 0x1000 || addr >= 0x80000000) continue;
        int duplicate = 0;
        for (unsigned j = 0; j < i; ++j) if (candidates[j] == addr) duplicate = 1;
        if (duplicate) continue;
        words(&w, "object", addr-16, 260);
        uint32_t child[8];
        if (lp32_crash_read(addr, child, sizeof(child)))
            for (unsigned j = 0; j < 8; ++j)
                if (child[j] >= 0x1000 && child[j] < 0x80000000)
                    words(&w, "indirect", child[j], 16);
    }
    events(&w, heap_events, HEAP_EVENTS, atomic_load_explicit(&heap_cursor, memory_order_acquire));
    events(&w, physics_events, PHYSICS_EVENTS, atomic_load_explicit(&physics_cursor, memory_order_acquire));
    string(&w, "capture end\n"); flush(&w);
}
