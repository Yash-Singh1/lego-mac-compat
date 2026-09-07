#include "regex_bridge.h"
#include <regex.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
/* regex_t embeds size_t and native pointers; regmatch_t contains two off_t
 * values and is 16 bytes in both Darwin ABIs. Keep compiled state native. */
struct regex32 {int32_t magic; uint32_t nsub,end,guts;};
struct entry {uint32_t guest; regex_t host; struct entry *next;};
static struct entry *entries;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
int regex_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    if(strcmp(name,"_regcomp")&&strcmp(name,"_regexec")&&strcmp(name,"_regfree")&&strcmp(name,"_regerror"))return 0;
    uint32_t address=!strcmp(name,"_regerror")?a[1]:a[0];
    pthread_mutex_lock(&lock);
    struct entry **slot=&entries;while(*slot&&(*slot)->guest!=address)slot=&(*slot)->next;
    struct entry *e=*slot;
    if(!strcmp(name,"_regcomp")) {
        if(e){regfree(&e->host);memset(&e->host,0,sizeof(e->host));}
        else {e=calloc(1,sizeof(*e));if(e){e->guest=address;e->next=entries;entries=e;}}
        if(!e)*out=REG_ESPACE;
        else {
            struct regex32 *g=(void *)(uintptr_t)address;
            if(a[2]&REG_PEND)e->host.re_endp=(void *)(uintptr_t)g->end;
            *out=regcomp(&e->host,(void *)(uintptr_t)a[1],(int)a[2]);
            g->magic=e->host.re_magic;g->nsub=(uint32_t)e->host.re_nsub;
            g->guts=address; /* opaque guest identity, never a native pointer */
        }
    } else if(!strcmp(name,"_regfree")) {
        if(e){regfree(&e->host);*slot=e->next;free(e);}*out=0;
    } else if(!strcmp(name,"_regexec")) {
        _Static_assert(sizeof(regmatch_t)==16,"Darwin regoff_t ABI");
        *out=e?regexec(&e->host,(void *)(uintptr_t)a[1],a[2],(void *)(uintptr_t)a[3],(int)a[4]):REG_BADPAT;
    } else *out=regerror((int)a[0],e?&e->host:NULL,(void *)(uintptr_t)a[2],a[3]);
    pthread_mutex_unlock(&lock);return 1;
}
