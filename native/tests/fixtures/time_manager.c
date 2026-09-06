#include <stdint.h>
#include <unistd.h>

#pragma pack(push, 2)
struct task { uint32_t next; int16_t type; uint32_t function; int32_t count, wakeup, reserved; };
#pragma pack(pop)
extern void *NewTimerUPP(void (*)(struct task *));
extern void DisposeTimerUPP(void *);
extern int16_t InstallTimeTask(struct task *);
extern int16_t PrimeTimeTask(struct task *, int32_t);
extern void PrimeTime(struct task *, int32_t);
extern int16_t RemoveTimeTask(struct task *);
static volatile int fires, entered, slow;
static struct task *expected;
static void fire(struct task *t)
{
    if (t != expected || (t->type & 0x8000)) { fires = -100; return; }
    entered = 1;
    if (slow) usleep(30000);
    ++fires;
    if (fires < 3) PrimeTime(t, -2000);
}
int check_time_manager(void)
{
    struct { struct task t; uint32_t guard; } record = {{0}, 0x12345678};
    fires = entered = slow = 0;
    expected = &record.t;
    void *upp = NewTimerUPP(fire);
    record.t.function = (uintptr_t)upp;
    if (!upp || InstallTimeTask(expected) || PrimeTimeTask(expected, 10)) return -250;
    for (int i = 0; i < 300 && fires >= 0 && fires < 3; ++i) usleep(10000);
    if (fires != 3 || RemoveTimeTask(expected) || record.guard != 0x12345678) return -251;
    /* Removing an active timer preserves its remaining delay and prevents it
       from calling a guest record after the caller frees it. */
    if (InstallTimeTask(expected) || PrimeTimeTask(expected, 1000) || RemoveTimeTask(expected) ||
        record.t.count >= 0 || (record.t.type & 0x8000)) return -252;
    usleep(20000);
    if (fires != 3) return -253;
    /* Remove also synchronizes with a callback already inside guest code,
       including a callback which tries to rearm itself during removal. */
    fires = entered = 0; slow = 1;
    if (InstallTimeTask(expected) || PrimeTimeTask(expected, 0)) return -254;
    for (int i = 0; i < 300 && !entered; ++i) usleep(1000);
    if (!entered || RemoveTimeTask(expected) || fires != 1) return -255;
    usleep(50000);
    if (fires != 1 || record.guard != 0x12345678) return -256;
    for (int i = 0; i < 140; ++i)
        if (InstallTimeTask(expected) || RemoveTimeTask(expected)) return -257;
    DisposeTimerUPP(upp);
    return 0;
}
