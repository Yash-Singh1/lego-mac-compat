#include "../src/regex_bridge.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
int main(void) {
    uint8_t *memory=mmap((void *)0x10000000,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
    assert(memory==(void *)0x10000000);
    struct regex32 *regex=(void *)memory;
    uint32_t *guard=(void *)(memory+sizeof(*regex));*guard=0xabcdef01;
    strcpy((char *)memory+64,"(brick)+");strcpy((char *)memory+128,"two brickbricks");
    uint32_t a[]={0x10000000,0x10000040,REG_EXTENDED,0,0};uint64_t out;
    assert(regex_bridge32_dispatch("_regcomp",a,&out)&&out==0);
    assert(regex->nsub==1&&*guard==0xabcdef01);
    a[1]=0x10000080;a[2]=2;a[3]=0x10000100;
    assert(regex_bridge32_dispatch("_regexec",a,&out)&&out==0);
    regmatch_t *matches=(void *)(memory+256);
    assert(matches[0].rm_so==4&&matches[0].rm_eo==14&&matches[1].rm_so==9);
    a[1]=0x10000040;assert(regex_bridge32_dispatch("_regexec",a,&out)&&out==0);
    assert(regex_bridge32_dispatch("_regfree",a,&out)&&!entries);
    puts("Regex bridge PASS (guest layout, capture offsets, cleanup)");
}
