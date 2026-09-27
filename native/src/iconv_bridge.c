#include "iconv_bridge.h"
#include "compat_runtime.h"
#include <errno.h>
#include <iconv.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct conversion { uint32_t token; iconv_t host; struct conversion *next; };
static struct conversion *conversions;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

int iconv_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    if(strcmp(name,"_iconv_open") && strcmp(name,"_iconv_close") &&
       strcmp(name,"_iconv") && strcmp(name,"_iconvctl"))return 0;
    if(!strcmp(name,"_iconv_open")) {
        iconv_t host=iconv_open((const char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1]);
        if(host==(iconv_t)-1){*out=UINT32_MAX;return 1;}
        struct conversion *entry=calloc(1,sizeof(*entry));
        uint32_t token=entry?compat_runtime32_allocate(4,1):0;
        if(!token){free(entry);iconv_close(host);errno=ENOMEM;*out=UINT32_MAX;return 1;}
        entry->token=token;entry->host=host;
        pthread_mutex_lock(&lock);entry->next=conversions;conversions=entry;pthread_mutex_unlock(&lock);
        *out=token;return 1;
    }
    pthread_mutex_lock(&lock);
    struct conversion **slot=&conversions;while(*slot && (*slot)->token!=a[0])slot=&(*slot)->next;
    struct conversion *entry=*slot;
    if(!entry){errno=EBADF;*out=UINT32_MAX;}
    else if(!strcmp(name,"_iconv_close")) {
        *out=(uint32_t)iconv_close(entry->host);*slot=entry->next;
        compat_runtime32_deallocate(entry->token);free(entry);
    } else if(!strcmp(name,"_iconvctl")) {
        /* Integer requests have the same layout. Callback-bearing requests
           need guest trampolines, so keep them on the unsupported path. */
        if(a[1]>4 && a[1]!=128 && a[1]!=129){pthread_mutex_unlock(&lock);return 0;}
        *out=(uint32_t)iconvctl(entry->host,(int)a[1],(void *)(uintptr_t)a[2]);
    } else {
        uint32_t *input=(void *)(uintptr_t)a[1],*in_size=(void *)(uintptr_t)a[2];
        uint32_t *output=(void *)(uintptr_t)a[3],*out_size=(void *)(uintptr_t)a[4];
        char *in=input?(void *)(uintptr_t)*input:NULL,*dest=output?(void *)(uintptr_t)*output:NULL;
        size_t left=in_size?*in_size:0,space=out_size?*out_size:0;
        *out=(uint32_t)iconv(entry->host,input?&in:NULL,in_size?&left:NULL,
                             output?&dest:NULL,out_size?&space:NULL);
        if(input)*input=(uint32_t)(uintptr_t)in;
        if(in_size)*in_size=(uint32_t)left;
        if(output)*output=(uint32_t)(uintptr_t)dest;
        if(out_size)*out_size=(uint32_t)space;
    }
    pthread_mutex_unlock(&lock);return 1;
}
