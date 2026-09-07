#include "../src/tlv_bridge.c"
#include <assert.h>
#include <stdio.h>
static uint32_t cursor=0x20000000;
uint32_t compat_runtime32_allocate(size_t n,int clear) {uint32_t p=__atomic_fetch_add(&cursor,(n+15)&~15u,__ATOMIC_RELAXED);if(clear)memset((void *)(uintptr_t)p,0,n);return p;}
void compat_runtime32_deallocate(uint32_t p) {(void)p;}
uint32_t compat_runtime32_guest_callback(const char *s) {(void)s;return 0x21000000;}
static uint32_t descriptor=0x20001000;
static void *worker(void *unused) {
    (void)unused;uint64_t value;assert(tlv_bridge32_dispatch("_lp32_tlv_get_addr",&descriptor,&value));
    uint32_t *p=(void *)(uintptr_t)value;assert(p[0]==123&&p[1]==0);p[0]=456;
    uint64_t again;assert(tlv_bridge32_dispatch("_lp32_tlv_get_addr",&descriptor,&again)&&again==value);
    return (void *)(uintptr_t)value;
}
int main(void) {
    assert(mmap((void *)0x20000000,1<<20,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0)==(void *)0x20000000);
    cursor=0x20002000;
    struct {struct mach_header header;struct segment_command segment;struct section sections[3];} image={0};
    image.header.ncmds=1;image.segment.cmd=LC_SEGMENT;image.segment.cmdsize=sizeof(image)-sizeof(image.header);image.segment.nsects=3;
    image.sections[0]=(struct section){.addr=0x20000000,.size=4,.align=4,.flags=S_THREAD_LOCAL_REGULAR};
    image.sections[1]=(struct section){.addr=0x20000004,.size=4,.align=2,.flags=S_THREAD_LOCAL_ZEROFILL};
    image.sections[2]=(struct section){.addr=descriptor,.size=12,.flags=S_THREAD_LOCAL_VARIABLES};
    *(uint32_t *)0x20000000=123;
    struct macho_image32 guest={.header=&image.header};assert(!tlv_bridge32_initialize(&guest));
    assert(*(uint32_t *)(uintptr_t)descriptor!=0);
    uint64_t value;assert(tlv_bridge32_dispatch("_lp32_tlv_get_addr",&descriptor,&value));
    assert(*(uint32_t *)(uintptr_t)value==123);*(uint32_t *)(uintptr_t)value=789;
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));void *other;assert(!pthread_join(thread,&other));
    assert((uintptr_t)other!=value&&*(uint32_t *)(uintptr_t)value==789);
    puts("TLV bridge PASS (descriptor setup, template bytes, zero storage, thread isolation)");
}
