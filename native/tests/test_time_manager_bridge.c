#include "time_manager_bridge.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static atomic_uint fired;
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *args, size_t count) {
    assert(function == 7 && count == 1 && args[0] == 0x10000000);
    atomic_fetch_add(&fired, 1);
    return 0;
}
static int32_t call(const char *name, uint32_t *args) {
    uint64_t out;
    assert(time_manager_bridge32_dispatch(name, args, &out));
    return (int32_t)out;
}
int main(void) {
    unsigned char *task = mmap((void *)0x10000000,4096,PROT_READ|PROT_WRITE,
                               MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
    assert(task == (void *)0x10000000);
    uint32_t callback[] = {7};
    uint32_t function = call("_NewTimerUPP",callback);
    memcpy(task+6,&function,4);
    memset(task+22,0xcc,8);
    uint32_t args[] = {0x10000000,10};
    assert(!call("_InstallTimeTask",args));
    assert(!call("_PrimeTime",args));
    for(unsigned i=0;i<2000 && !atomic_load(&fired);++i)usleep(1000);
    assert(atomic_load(&fired)==1);
    args[1]=1000;assert(!call("_PrimeTimeTask",args));
    assert(!call("_RemoveTimeTask",args));
    usleep(50000);assert(atomic_load(&fired)==1);
    for(unsigned i=22;i<30;++i)assert(task[i]==0xcc);
    assert(!call("_DisposeTimerUPP",callback));
    munmap(task,4096);
    puts("Time Manager PASS (packed guest record, callback, rearm, cancellation)");
}
