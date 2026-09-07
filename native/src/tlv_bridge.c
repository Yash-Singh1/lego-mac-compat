#include "tlv_bridge.h"
#include "compat_runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <limits.h>

static uint32_t template_base,template_size,template_alignment=16;
static unsigned char *template_bytes;
static pthread_key_t storage_key;
struct storage { uint32_t allocation,base; };
static void destroy_storage(void *value) {
    struct storage *s=value;if(s){compat_runtime32_deallocate(s->allocation);free(s);}
}
int tlv_bridge32_initialize(const struct macho_image32 *image) {
    const uint8_t *commands=(const void *)(image->header+1);
    uint32_t low=UINT32_MAX,high=0,descriptors=0;
    for(uint32_t i=0;i<image->header->ncmds;++i) {
        const struct load_command *lc=(const void *)commands;
        if(lc->cmd==LC_SEGMENT) {
            const struct segment_command *seg=(const void *)commands;
            const struct section *s=(const void *)(seg+1);
            for(uint32_t j=0;j<seg->nsects;++j) {
                uint32_t kind=s[j].flags&SECTION_TYPE;
                if(kind==S_THREAD_LOCAL_REGULAR || kind==S_THREAD_LOCAL_ZEROFILL) {
                    if(s[j].addr<low)low=s[j].addr;
                    if(s[j].size>UINT32_MAX-s[j].addr)return -1;
                    if(s[j].addr+s[j].size>high)high=s[j].addr+s[j].size;
                    if(s[j].align>20)return -1;
                    if((1u<<s[j].align)>template_alignment)template_alignment=1u<<s[j].align;
                } else if(kind==S_THREAD_LOCAL_VARIABLES)descriptors+=s[j].size/12;
                else if(kind==S_THREAD_LOCAL_INIT_FUNCTION_POINTERS && s[j].size)return -1;
            }
        }
        commands+=lc->cmdsize;
    }
    if(!descriptors)return 0;
    if(low==UINT32_MAX || high<=low)return -1;
    template_base=low;template_size=high-low;template_bytes=malloc(template_size);
    if(!template_bytes || pthread_key_create(&storage_key,destroy_storage))return -1;
    memcpy(template_bytes,(void *)(uintptr_t)low,template_size);
    uint32_t allocation=compat_runtime32_allocate(8192,1);
    if(!allocation)return -1;
    uint32_t code=(allocation+4095)&~4095u;
    // Darwin i386 TLV access passes its descriptor in EAX and preserves all
    // other registers, including floating-point/SIMD state.
    uint8_t bytes[]={0x51,0x52,0x55,0x89,0xe5,0x83,0xe4,0xf0,
        0x81,0xec,0x10,0x02,0,0,0x0f,0xae,0x44,0x24,0x10,
        0x89,0x04,0x24,0xe8,0,0,0,0,
        0x0f,0xae,0x4c,0x24,0x10,0x89,0xec,0x5d,0x5a,0x59,0xc3};
    uint32_t thunk=compat_runtime32_guest_callback("_lp32_tlv_get_addr");
    if(!thunk)return -1;
    int32_t delta=(int32_t)(thunk-(code+27));memcpy(bytes+23,&delta,4);
    memcpy((void *)(uintptr_t)code,bytes,sizeof(bytes));
    if(mprotect((void *)(uintptr_t)code,4096,PROT_READ|PROT_EXEC))return -1;
    commands=(const void *)(image->header+1);
    for(uint32_t i=0;i<image->header->ncmds;++i) {
        const struct load_command *lc=(const void *)commands;
        if(lc->cmd==LC_SEGMENT) {
            const struct segment_command *seg=(const void *)commands;const struct section *s=(const void *)(seg+1);
            for(uint32_t j=0;j<seg->nsects;++j)if((s[j].flags&SECTION_TYPE)==S_THREAD_LOCAL_VARIABLES) {
                uint32_t *d=(void *)(uintptr_t)s[j].addr;
                for(uint32_t k=0;k+12<=s[j].size;k+=12,d+=3){if(d[2]>=template_size)return -1;d[0]=code;}
            }
        }
        commands+=lc->cmdsize;
    }
    return 0;
}
int tlv_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    if(strcmp(name,"_lp32_tlv_get_addr"))return 0;
    if(!template_base || !a[0])return 0;
    uint32_t offset=((const uint32_t *)(uintptr_t)a[0])[2];if(offset>=template_size)return 0;
    struct storage *s=pthread_getspecific(storage_key);
    if(!s) {
        s=calloc(1,sizeof(*s));if(!s)return 0;
        s->allocation=compat_runtime32_allocate((size_t)template_size+template_alignment-1,0);
        if(!s->allocation){free(s);return 0;}
        s->base=(s->allocation+template_alignment-1)&~(template_alignment-1);
        memcpy((void *)(uintptr_t)s->base,template_bytes,template_size);
        if(pthread_setspecific(storage_key,s)){destroy_storage(s);return 0;}
    }
    *out=s->base+offset;return 1;
}
