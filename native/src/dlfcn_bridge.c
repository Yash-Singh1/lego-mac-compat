#include "dlfcn_bridge.h"
#include "compat_runtime.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct library {uint32_t token;void *native;struct library *next;};
static struct library *libraries;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char error_text[2048];
static _Thread_local int error_pending;
static _Thread_local uint32_t guest_error;
static void error(const char *text){snprintf(error_text,sizeof(error_text),"%s",text?text:"dynamic loader error");error_pending=1;}
static void *lookup(uint32_t token){
    if((int32_t)token==-1||(int32_t)token==-2||(int32_t)token==-3||(int32_t)token==-5)return (void *)(intptr_t)(int32_t)token;
    void *native=NULL;pthread_mutex_lock(&lock);
    for(struct library *p=libraries;p;p=p->next)if(p->token==token){native=p->native;break;}
    pthread_mutex_unlock(&lock);return native;
}
int dlfcn_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if(!strcmp(name,"_dlopen")){
        const char *path=(void *)(uintptr_t)a[0];void *native=dlopen(path,(int)a[1]);
        if(getenv("LP32_TRACE_DYLD"))fprintf(stderr,"compat32: dlopen %s -> %s\n",path?path:"(main executable)",native?"loaded":"failed");
        if(!native){error(dlerror());*out=0;return 1;}
        struct library *p=calloc(1,sizeof(*p));uint32_t token=p?compat_runtime32_allocate(4,1):0;
        if(!token){free(p);dlclose(native);error("cannot allocate a guest library handle");*out=0;return 1;}
        p->token=token;p->native=native;pthread_mutex_lock(&lock);p->next=libraries;libraries=p;pthread_mutex_unlock(&lock);*out=token;return 1;
    }
    if(!strcmp(name,"_dlsym")){
        const char *symbol=(void *)(uintptr_t)a[1];void *native=lookup(a[0]);
        if(!native||!symbol){error("invalid guest library handle or symbol name");*out=0;return 1;}
        dlerror();void *value=dlsym(native,symbol);const char *failure=dlerror();
        if(getenv("LP32_TRACE_DYLD"))fprintf(stderr,"compat32: dlsym %s -> %s\n",symbol,failure?"missing":"found");
        if(failure){error(failure);*out=0;return 1;}
        if(!value){*out=0;return 1;}
        char imported[96];if(snprintf(imported,sizeof(imported),"_%s",symbol)>=(int)sizeof(imported)){error("symbol name exceeds guest thunk capacity");*out=0;return 1;}
        /* The thunk performs the typed ABI conversion. A native function
           pointer must never be returned directly to compatibility mode. */
        *out=compat_runtime32_guest_callback(imported);return 1;
    }
    if(!strcmp(name,"_dlclose")){
        pthread_mutex_lock(&lock);struct library **slot=&libraries;
        while(*slot&&(*slot)->token!=a[0])slot=&(*slot)->next;
        struct library *p=*slot;if(p)*slot=p->next;pthread_mutex_unlock(&lock);
        if(!p){error("invalid guest library handle");*out=(uint32_t)-1;return 1;}
        int status=dlclose(p->native);if(status)error(dlerror());compat_runtime32_deallocate(p->token);free(p);*out=(uint32_t)status;return 1;
    }
    if(!strcmp(name,"_dlerror")){
        if(!error_pending){*out=0;return 1;}
        if(guest_error)compat_runtime32_deallocate(guest_error);
        guest_error=compat_runtime32_copy_cstring(error_text);error_pending=0;*out=guest_error;return 1;
    }
    return 0;
}
