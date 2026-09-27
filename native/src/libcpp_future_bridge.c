#include "libcpp_future_bridge.h"
#include "compat_runtime.h"
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

/* Apple libc++ ABI v1, verified against MW2's inline packaged_task accessors.
   Keep mutex/condition storage in the guest so inline template methods and
   imported promise<void> methods share the same synchronization objects. */
struct state32 {
    uint32_t vtable; int32_t owners; uint32_t exception;
    uint8_t mutex[44],condition[28]; uint32_t flags;
};
_Static_assert(sizeof(struct state32)==88 && offsetof(struct state32,flags)==84,"i386 future state");
static uint32_t *word(uint32_t p){return (void *)(uintptr_t)p;}
static struct state32 *state(uint32_t p){return (void *)(uintptr_t)p;}
static uint32_t call(const char *name,uint32_t a,uint32_t b) {
    uint32_t args[]={a,b};return (uint32_t)compat_runtime32_dispatch_import(name,args);
}
static void release(uint32_t p) {
    if(p && __atomic_sub_fetch(&state(p)->owners,1,__ATOMIC_ACQ_REL)==-1){
        uint32_t fn=word(state(p)->vtable)[2];compat_runtime32_call(fn,&p,1);
    }
}
static uint32_t vtable;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void initialize(void) {
    vtable=compat_runtime32_allocate(16,1);
    const char *names[]={"_lp32_future_destroy","_lp32_future_delete","_lp32_future_delete",
        "__ZNSt3__117__assoc_sub_state9__executeEv"};
    for(unsigned i=0;i<4 && vtable;++i)word(vtable)[i]=compat_runtime32_guest_callback(names[i]);
}
static bool wait_locked(uint32_t p,uint32_t *unique_lock) {
    struct state32 *s=state(p);
    if(!(s->flags&4) && (s->flags&8)) {
        s->flags&=~8u;
        call("_pthread_mutex_unlock",unique_lock[0],0);((uint8_t *)unique_lock)[4]=0;
        compat_runtime32_call(word(s->vtable)[3],&p,1);
        return !compat_runtime32_last_call_trapped();
    }
    while(!(s->flags&4))if(call("_pthread_cond_wait",p+56,unique_lock[0]))return false;
    return true;
}
int libcpp_future_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    *out=0;
    if(!strcmp(name,"_lp32_future_destroy") || !strcmp(name,"_lp32_future_delete")) {
        /* Exception propagation needs the guest exception runtime. */
        if(state(a[0])->exception)return 0;
        call("_pthread_cond_destroy",a[0]+56,0);call("_pthread_mutex_destroy",a[0]+12,0);
        if(!strcmp(name,"_lp32_future_delete"))compat_runtime32_deallocate(a[0]);
        return 1;
    }
    if(!strcmp(name,"__ZNSt3__17promiseIvEC1Ev")) {
        pthread_once(&once,initialize);
        uint32_t p=compat_runtime32_allocate(sizeof(struct state32),1);
        if(!p || !vtable)return 0;
        state(p)->vtable=vtable;*word(a[0])=p;return 1;
    }
    if(!strcmp(name,"__ZNSt3__17promiseIvE10get_futureEv")) {
        uint32_t p=*word(a[1]);if(!p)return 0;
        call("_pthread_mutex_lock",p+12,0);
        bool available=!(state(p)->flags&2);
        if(available){__atomic_add_fetch(&state(p)->owners,1,__ATOMIC_RELAXED);state(p)->flags|=2;*word(a[0])=p;}
        call("_pthread_mutex_unlock",p+12,0);*out=a[0];return available;
    }
    if(!strcmp(name,"__ZNSt3__17promiseIvE9set_valueEv")) {
        uint32_t p=*word(a[0]);if(!p)return 0;
        call("_pthread_mutex_lock",p+12,0);
        bool available=!(state(p)->flags&1) && !state(p)->exception;
        if(available){state(p)->flags|=5;call("_pthread_cond_broadcast",p+56,0);}
        call("_pthread_mutex_unlock",p+12,0);return available;
    }
    if(!strcmp(name,"__ZNSt3__16futureIvED1Ev") || !strcmp(name,"__ZNSt3__17promiseIvED1Ev")) {
        uint32_t p=*word(a[0]);
        if(p && !strcmp(name,"__ZNSt3__17promiseIvED1Ev") &&
           !(state(p)->flags&1) && !state(p)->exception && __atomic_load_n(&state(p)->owners,__ATOMIC_ACQUIRE)>0)return 0;
        release(p);*word(a[0])=0;return 1;
    }
    if(!strcmp(name,"__ZNSt3__16futureIvE3getEv")) {
        uint32_t p=*word(a[0]);if(!p)return 0;
        *word(a[0])=0;call("_pthread_mutex_lock",p+12,0);
        uint32_t unique_lock[]={p+12,1};bool ok=wait_locked(p,unique_lock) && !state(p)->exception;
        if(unique_lock[1])call("_pthread_mutex_unlock",p+12,0);
        release(p);return ok;
    }
    if(!strcmp(name,"__ZNSt3__117__assoc_sub_state10__sub_waitERNS_11unique_lockINS_5mutexEEE"))
        return wait_locked(a[0],word(a[1]));
    return 0;
}
