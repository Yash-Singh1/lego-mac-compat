#include "time_manager_bridge.h"
#include "compat_runtime.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Time Manager still exists in the host, but TMTask has native pointers and
   longs. Keep a native record and pass the original, packed i386 record back
   to Bink's asynchronous I/O timer. Never expose a native UPP to the guest. */
#pragma pack(push, 2)
struct task32 { uint32_t next; int16_t type; uint32_t function; int32_t count, wakeup, reserved; };
#pragma pack(pop)
_Static_assert(sizeof(struct task32) == 22, "i386 TMTask");
struct timer {
    TMTask host;
    uint32_t guest;
    bool installed, removing;
    unsigned running, operations;
};
static struct timer timers[128];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local struct timer *current_timer;

static void copy_state(struct timer *t)
{
    struct task32 *g = (void *)(uintptr_t)t->guest;
    g->type = t->host.qType;
    g->count = (int32_t)t->host.tmCount;
    g->wakeup = (int32_t)t->host.tmWakeUp;
    /* qLink and tmReserved are private native pointers/internal data. */
}

static void timer_fired(TMTaskPtr task)
{
    struct timer *t = (void *)task;
    pthread_mutex_lock(&lock);
    uint32_t function = 0, guest = t->guest;
    if (t->installed && !t->removing) {
        copy_state(t);
        function = ((struct task32 *)(uintptr_t)guest)->function;
        ++t->running;
    } else { pthread_mutex_unlock(&lock); return; }
    pthread_mutex_unlock(&lock);
    current_timer = t;
    if (function) compat_runtime32_call(function, &guest, 1);
    current_timer = NULL;
    pthread_mutex_lock(&lock);
    --t->running;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
int time_manager_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (!strcmp(name, "_NewTimerUPP")) { *result = a[0]; return 1; }
    if (!strcmp(name, "_DisposeTimerUPP")) { *result = 0; return 1; }
    bool install = !strcmp(name, "_InstallTimeTask");
    bool prime = !strcmp(name, "_PrimeTime") || !strcmp(name, "_PrimeTimeTask");
    bool remove = !strcmp(name, "_RemoveTimeTask");
    if (!install && !prime && !remove) return 0;
    OSErr status = -50;
    if (!a[0]) goto done;
    pthread_mutex_lock(&lock);
    struct timer *t = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < 128; ++i) {
        if (timers[i].guest == a[0] && (timers[i].installed || timers[i].running || timers[i].operations || timers[i].removing)) t = &timers[i];
        if (!timers[i].installed && !timers[i].running && !timers[i].operations && !timers[i].removing && !free_slot) free_slot = &timers[i];
    }
    if (install) {
        if (t || !free_slot) { pthread_mutex_unlock(&lock); goto done; }
        t = free_slot;
        memset(t, 0, sizeof(*t));
        t->guest = a[0]; t->installed = true; t->operations = 1;
        t->host.tmAddr = timer_fired;
        t->host.tmCount = ((struct task32 *)(uintptr_t)a[0])->count;
        pthread_mutex_unlock(&lock);
        status = InstallTimeTask((QElemPtr)&t->host);
        pthread_mutex_lock(&lock);
        if (status) t->installed = false;
        else copy_state(t);
        --t->operations;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    } else if (!t || !t->installed || t->removing) pthread_mutex_unlock(&lock);
    else if (prime) {
        ++t->operations;
        pthread_mutex_unlock(&lock);
        status = PrimeTimeTask((QElemPtr)&t->host, (int32_t)a[1]);
        pthread_mutex_lock(&lock);
        if (t->installed && !t->removing) copy_state(t);
        --t->operations;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    } else {
        t->removing = true;
        while (t->operations) pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
        status = RemoveTimeTask((QElemPtr)&t->host);
        pthread_mutex_lock(&lock);
        while (t->running && current_timer != t) pthread_cond_wait(&changed, &lock);
        if (!status) { copy_state(t); t->installed = false; }
        t->removing = false;
        pthread_mutex_unlock(&lock);
    }
done:
    *result = (uint32_t)(int32_t)status;
    return 1;
}
#pragma clang diagnostic pop
