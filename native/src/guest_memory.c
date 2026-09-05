#include "guest_memory.h"
#include "compat_runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { PAGE_SIZE32 = 4096, MAPPING_LIMIT = 256 };
struct mapping32 {
    uint32_t allocation, base, length, live_pages;
    unsigned char *live;
};
static struct mapping32 mappings[MAPPING_LIMIT];
static pthread_mutex_t mapping_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rounded_length(uint32_t length)
{
    if (!length || length > UINT32_MAX - PAGE_SIZE32 + 1) return 0;
    return (length + PAGE_SIZE32 - 1) & ~(uint32_t)(PAGE_SIZE32 - 1);
}

/* Caller holds mapping_lock. The enclosing heap allocation remains claimed
   until every page is unmapped, including when a reservation is trimmed. */
static struct mapping32 *owner(uint32_t address, uint32_t length)
{
    if ((address & (PAGE_SIZE32 - 1)) || !length) return NULL;
    for (unsigned i = 0; i < MAPPING_LIMIT; ++i) {
        struct mapping32 *m = mappings + i;
        if (m->allocation && address >= m->base && address - m->base <= m->length &&
            length <= m->length - (address - m->base)) return m;
    }
    return NULL;
}

static bool live_range(const struct mapping32 *m, uint32_t address, uint32_t length)
{
    unsigned first = (address - m->base) / PAGE_SIZE32;
    for (unsigned i = 0; i < length / PAGE_SIZE32; ++i) if (!m->live[first + i]) return false;
    return true;
}

uint32_t guest_memory32_size(uint32_t address)
{
    pthread_mutex_lock(&mapping_lock);
    struct mapping32 *m = owner(address, PAGE_SIZE32);
    uint32_t size = m && m->base == address ? m->length : 0;
    pthread_mutex_unlock(&mapping_lock);
    return size;
}

uint32_t guest_memory32_map(uint32_t hint, uint32_t length, int protection,
                            int flags, int descriptor, int64_t offset)
{
    uint32_t size = rounded_length(length);
    if (!size || size > UINT32_MAX - 2 * PAGE_SIZE32) { errno = EINVAL; return UINT32_MAX; }
    pthread_mutex_lock(&mapping_lock);
    struct mapping32 *m = NULL;
    bool fresh = !(flags & MAP_FIXED);
    if (!fresh) m = owner(hint, size);
    else {
        for (unsigned i = 0; i < MAPPING_LIMIT; ++i) if (!mappings[i].allocation) { m = mappings + i; break; }
        if (m) {
            unsigned char *live = calloc(size / PAGE_SIZE32, 1);
            uint32_t allocation = live ? compat_runtime32_allocate((size_t)size + 2 * PAGE_SIZE32, 0) : 0;
            if (!allocation) { free(live); m = NULL; }
            else {
                /* Padding keeps both the heap header and the footer read by
                   adjacent free-block coalescing outside protected pages. */
                *m = (struct mapping32){allocation,
                    (allocation + PAGE_SIZE32 - 1) & ~(uint32_t)(PAGE_SIZE32 - 1), size, 0, live};
                hint = m->base;
            }
        }
    }
    if (!m) { pthread_mutex_unlock(&mapping_lock); errno = ENOMEM; return UINT32_MAX; }
    void *result = mmap((void *)(uintptr_t)hint, size, protection,
                        flags | MAP_FIXED, descriptor, offset);
    int saved_errno = errno;
    if (result == MAP_FAILED && fresh) {
        /* Re-establish ordinary heap pages before returning the allocation. */
        if (mmap((void *)(uintptr_t)m->base, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED) {
            compat_runtime32_deallocate(m->allocation);
            free(m->live);
            memset(m, 0, sizeof(*m));
        }
    } else if (result != MAP_FAILED) {
        unsigned first = (hint - m->base) / PAGE_SIZE32;
        for (unsigned i = 0; i < size / PAGE_SIZE32; ++i) {
            if (!m->live[first + i]) ++m->live_pages;
            m->live[first + i] = 1;
        }
    }
    pthread_mutex_unlock(&mapping_lock);
    errno = saved_errno;
    return result == MAP_FAILED ? UINT32_MAX : hint;
}

int guest_memory32_unmap(uint32_t address, uint32_t length)
{
    uint32_t size = rounded_length(length);
    pthread_mutex_lock(&mapping_lock);
    struct mapping32 *m = owner(address, size);
    if (!m) { pthread_mutex_unlock(&mapping_lock); errno = EINVAL; return -1; }
    /* Keep the process's low-address reservation intact. PROT_NONE provides
       unmapped-page behavior without making the range available to the host. */
    int result = mmap((void *)(uintptr_t)address, size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED ? -1 : 0;
    if (!result) {
        unsigned first = (address - m->base) / PAGE_SIZE32;
        for (unsigned i = 0; i < size / PAGE_SIZE32; ++i) {
            if (m->live[first + i]) --m->live_pages;
            m->live[first + i] = 0;
        }
        if (!m->live_pages) {
            if (mmap((void *)(uintptr_t)m->base, m->length, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) result = -1;
            else {
                compat_runtime32_deallocate(m->allocation);
                free(m->live);
                memset(m, 0, sizeof(*m));
            }
        }
    }
    pthread_mutex_unlock(&mapping_lock);
    return result;
}

static int operate(uint32_t address, uint32_t length, int value, unsigned operation)
{
    uint32_t size = rounded_length(length);
    pthread_mutex_lock(&mapping_lock);
    struct mapping32 *m = owner(address, size);
    int result;
    if (!m || !live_range(m, address, size)) { errno = ENOMEM; result = -1; }
    else if (operation == 0) result = mprotect((void *)(uintptr_t)address, size, value);
    else if (operation == 1) result = madvise((void *)(uintptr_t)address, size, value);
    else result = msync((void *)(uintptr_t)address, size, value);
    pthread_mutex_unlock(&mapping_lock);
    return result;
}

int guest_memory32_protect(uint32_t address, uint32_t length, int protection) { return operate(address, length, protection, 0); }
int guest_memory32_advise(uint32_t address, uint32_t length, int advice) { return operate(address, length, advice, 1); }
int guest_memory32_sync(uint32_t address, uint32_t length, int flags) { return operate(address, length, flags, 2); }
