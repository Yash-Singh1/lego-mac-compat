#include "blocks_bridge.h"
#include "compat_runtime.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
enum { NEEDS_FREE=1u<<24,HAS_HELPERS=1u<<25,GLOBAL=1u<<28,REFMASK=0xffff };
struct block32 {uint32_t isa,flags,reserved,invoke,descriptor;};
struct byref32 {uint32_t isa,forward,flags,size,keep,destroy;};
static void *ptr(uint32_t p){return (void *)(uintptr_t)p;}
uint32_t blocks_bridge32_copy(uint32_t token){
    if(!token)return 0;struct block32 *b=ptr(token);
    if(b->flags&GLOBAL)return token;
    if(b->flags&NEEDS_FREE){__atomic_add_fetch(&b->flags,1,__ATOMIC_SEQ_CST);return token;}
    const uint32_t *d=ptr(b->descriptor);uint32_t size=d[1];if(size<sizeof(*b)||size>16*1024*1024)return 0;
    uint32_t copy=compat_runtime32_allocate(size,0);if(!copy)return 0;
    memcpy(ptr(copy),b,size);struct block32 *c=ptr(copy);c->flags=(c->flags&~REFMASK)|NEEDS_FREE|1;
    if(b->flags&HAS_HELPERS){uint32_t a[]={copy,token};compat_runtime32_call(d[2],a,2);}return copy;
}
void blocks_bridge32_release(uint32_t token){
    if(!token)return;struct block32 *b=ptr(token);if(!(b->flags&NEEDS_FREE)||b->flags&GLOBAL)return;
    if((__atomic_sub_fetch(&b->flags,1,__ATOMIC_SEQ_CST)&REFMASK)==0){
        if(b->flags&HAS_HELPERS){const uint32_t *d=ptr(b->descriptor);compat_runtime32_call(d[3],&token,1);}
        compat_runtime32_deallocate(token);
    }
}
uint32_t blocks_bridge32_invoke(uint32_t token){struct block32 *b=ptr(token);return token?compat_runtime32_call(b->invoke,&token,1):0;}
static uint32_t byref_copy(uint32_t token){
    struct byref32 *b=ptr(token);if(b->forward)b=ptr(b->forward);
    if(b->flags&NEEDS_FREE){__atomic_add_fetch(&b->flags,1,__ATOMIC_SEQ_CST);return (uint32_t)(uintptr_t)b;}
    if(b->size<16||b->size>16*1024*1024)return 0;
    uint32_t copy=compat_runtime32_allocate(b->size,0);if(!copy)return 0;
    struct byref32 *c=ptr(copy);memcpy(c,b,b->size);c->flags=(c->flags&~REFMASK)|NEEDS_FREE|2;c->forward=copy;b->forward=copy;
    if(c->flags&HAS_HELPERS){uint32_t a[]={copy,(uint32_t)(uintptr_t)b};compat_runtime32_call(c->keep,a,2);}return copy;
}
static void byref_release(uint32_t token){
    if(!token)return;struct byref32 *b=ptr(token);if(b->forward)b=ptr(b->forward);
    if(!(b->flags&NEEDS_FREE))return;
    if((__atomic_sub_fetch(&b->flags,1,__ATOMIC_SEQ_CST)&REFMASK)==0){
        uint32_t p=(uint32_t)(uintptr_t)b;if(b->flags&HAS_HELPERS)compat_runtime32_call(b->destroy,&p,1);
        compat_runtime32_deallocate(p);
    }
}
int blocks_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    *out=0;
    if(!strcmp(name,"__Block_copy"))*out=blocks_bridge32_copy(a[0]);
    else if(!strcmp(name,"__Block_release"))blocks_bridge32_release(a[0]);
    else if(!strcmp(name,"__Block_object_assign")){
        uint32_t p=a[1];unsigned kind=a[2]&15;
        if(kind==8)p=byref_copy(p);else if(kind==7)p=blocks_bridge32_copy(p);
        else if(kind==3 && !(a[2]&16))compat_runtime32_dispatch_import("_CFRetain",&p);
        *(uint32_t *)ptr(a[0])=p;
    } else if(!strcmp(name,"__Block_object_dispose")){
        unsigned kind=a[1]&15;
        if(kind==8)byref_release(a[0]);else if(kind==7)blocks_bridge32_release(a[0]);
        else if(kind==3 && !(a[1]&16))compat_runtime32_dispatch_import("_CFRelease",a);
    } else return 0;
    return 1;
}
