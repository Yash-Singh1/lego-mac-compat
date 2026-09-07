#include "dlfcn_bridge.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t next=0x10001000;
uint32_t compat_runtime32_allocate(size_t size,int clear){uint32_t p=next;next+=(size+15)&~15u;assert(next<0x10010000);if(clear)memset((void *)(uintptr_t)p,0,size);return p;}
void compat_runtime32_deallocate(uint32_t p){assert(p>=0x10001000&&p<next);}
uint32_t compat_runtime32_copy_cstring(const char *s){uint32_t p=compat_runtime32_allocate(strlen(s)+1,0);strcpy((void *)(uintptr_t)p,s);return p;}
uint32_t compat_runtime32_guest_callback(const char *symbol){assert(!strcmp(symbol,"_strlen"));return 0x7f005000;}
static uint64_t call(const char *name,uint32_t *a){uint64_t out;assert(dlfcn_bridge32_dispatch(name,a,&out));return out;}
int main(void){
    char *memory=mmap((void *)0x10000000,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);assert(memory==(void *)0x10000000);
    strcpy(memory,"/usr/lib/libSystem.B.dylib");strcpy(memory+64,"strlen");strcpy(memory+128,"lp32_symbol_that_does_not_exist");
    uint32_t a[]={0x10000000,RTLD_NOW|RTLD_LOCAL};uint32_t handle=(uint32_t)call("_dlopen",a);assert(handle>=0x10001000);
    a[0]=handle;a[1]=0x10000040;assert(call("_dlsym",a)==0x7f005000);
    a[1]=0x10000080;assert(!call("_dlsym",a));uint32_t message=(uint32_t)call("_dlerror",a);assert(message&&strstr((char *)(uintptr_t)message,"lp32_symbol_that_does_not_exist"));assert(!call("_dlerror",a));
    assert(!call("_dlclose",a));assert((int32_t)call("_dlclose",a)==-1);assert(call("_dlerror",a));
    strcpy(memory,"/lp32/nonexistent-library.dylib");a[0]=0x10000000;a[1]=RTLD_NOW;assert(!call("_dlopen",a));assert(call("_dlerror",a));
    puts("Dynamic loader bridge PASS (real handles, typed symbol thunk, missing library/symbol, error lifetime, close)");
}
