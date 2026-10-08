#include "../src/libcpp_bridge.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <wchar.h>
static uint32_t cursor=0x30001000;
static unsigned allocations;
uint32_t compat_runtime32_allocate(size_t n,int clear){++allocations;uint32_t p=cursor;cursor+=(n+15)&~15u;assert(cursor<0x30100000);if(clear)memset((void *)(uintptr_t)p,0,n);return p;}
void compat_runtime32_deallocate(uint32_t p){assert(p>=0x30001000 && p<cursor);}
static unsigned shared_destroyed,weak_destroyed;
uint32_t compat_runtime32_call(uint32_t function,const uint32_t *a,size_t count){
    assert(count==1&&a[0]==0x30000200);
    if(function==1)++shared_destroyed;else {assert(function==2);++weak_destroyed;}return 0;
}
static unsigned traps;
void compat_runtime32_trap_import(const char *name,const uint32_t *a,uint32_t caller){(void)name;(void)a;(void)caller;++traps;}
static const char *prefix="__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
static uint32_t run(const char *suffix,uint32_t *a){char name[256];snprintf(name,sizeof(name),"%s%s",prefix,suffix);uint64_t r;assert(libcpp_bridge32_dispatch(name,a,&r));return r;}
static unsigned length(uint32_t *s){return s[0]&1?s[1]:*(uint8_t *)s>>1;}
static char *data(uint32_t *s){return s[0]&1?(void *)(uintptr_t)s[2]:(char *)s+1;}
int main(void){
    uint32_t *s=mmap((void *)0x30000000,0x100000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);assert(s!=MAP_FAILED);
    char *input=(char *)s+128;memcpy(input,"abcdef",7);
    uint32_t a[8]={(uint32_t)(uintptr_t)s,(uint32_t)(uintptr_t)input,6,0};
    run("6__initEPKcm",a);assert(!(s[0]&1)&&length(s)==6&&!strcmp(data(s),"abcdef"));
    a[1]=(uint32_t)(uintptr_t)data(s);a[2]=6;run("6appendEPKcm",a);
    assert((s[0]&1)&&length(s)==12&&!strcmp(data(s),"abcdefabcdef"));
    {
        uint64_t found;
        char *needle=(char *)s+256;memcpy(needle,"cde",3);
        uint32_t find_args[]={(uint32_t)(uintptr_t)s,(uint32_t)(uintptr_t)needle,0,3};
        const char *name="__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEPKcmm";
        assert(libcpp_bridge32_dispatch(name,find_args,&found)&&found==2);
        find_args[2]=3;assert(libcpp_bridge32_dispatch(name,find_args,&found)&&found==8);
        find_args[2]=12;find_args[3]=0;
        assert(libcpp_bridge32_dispatch(name,find_args,&found)&&found==12);
        find_args[3]=3;assert(libcpp_bridge32_dispatch(name,find_args,&found)&&found==UINT32_MAX);
    }
    {
        uint32_t *replace_string=s+240;
        uint32_t init[]={(uint32_t)(uintptr_t)replace_string,(uint32_t)(uintptr_t)input,6};
        run("6__initEPKcm",init);
        memcpy(input,"XYZ",3);
        uint32_t args[]={(uint32_t)(uintptr_t)replace_string,2,3,(uint32_t)(uintptr_t)input,3};
        assert(run("7replaceEmmPKcm",args)==args[0]);
        assert(!strcmp(data(replace_string),"abXYZf"));
        run("D1Ev",args);
        memcpy(input,"abcdef",7);
    }
    a[1]=2;a[2]=UINT32_MAX;run("5eraseEmm",a);assert(length(s)==2&&!strcmp(data(s),"ab"));
    memcpy(input,"x\0y",3);a[1]=1;a[2]=(uint32_t)(uintptr_t)input;a[3]=3;run("6insertEmPKcm",a);
    assert(length(s)==5&&!memcmp(data(s),"ax\0yb\0",6));
    uint32_t *copy=s+8;a[0]=(uint32_t)(uintptr_t)copy;a[1]=(uint32_t)(uintptr_t)s;run("C1ERKS5_",a);
    assert(length(copy)==5&&!memcmp(data(copy),data(s),6));
    uint32_t *sub=s+12;a[0]=(uint32_t)(uintptr_t)sub;a[2]=1;a[3]=UINT32_MAX;
    run("C1ERKS5_mmRKS4_",a);assert(length(sub)==4&&!memcmp(data(sub),"x\0yb\0",5));
    run("D1Ev",a);a[2]=5;a[3]=12;run("C2ERKS5_mmRKS4_",a);assert(!length(sub)&&!data(sub)[0]);
    run("D1Ev",a);a[2]=0;a[3]=2;run("C1ERKS5_mmRKS4_",a);assert(length(sub)==2&&!strcmp(data(sub),"ax"));
    run("D1Ev",a);
    a[0]=(uint32_t)(uintptr_t)s;a[1]=7;a[2]='z';run("6resizeEmc",a);
    assert(length(s)==7&&!memcmp(data(s),"ax\0ybzz\0",8));assert(length(copy)==5);
    a[1]=200;run("7reserveEm",a);assert(length(s)==7&&(s[0]&~1u)>200);
    a[1]='!';run("9push_backEc",a);assert(length(s)==8&&data(s)[7]=='!'&&data(s)[8]==0);
    run("D1Ev",a);assert(!s[0]&&!s[1]&&!s[2]);
    a[0]=(uint32_t)(uintptr_t)copy;run("D1Ev",a);
    /* Shader-sized strings built a character at a time must grow in O(log N)
       allocations. Check every byte, including aliases across reallocation. */
    a[0]=(uint32_t)(uintptr_t)s;
    unsigned before_allocations=allocations;
    for(unsigned i=0;i<32768;++i){a[1]='a'+i%26;run("9push_backEc",a);}
    assert(allocations-before_allocations<16 && length(s)==32768);
    for(unsigned i=0;i<32768;++i)assert(data(s)[i]=='a'+i%26);
    assert(!data(s)[32768]);
    a[1]=(uint32_t)(uintptr_t)data(s);a[2]=32768;run("6appendEPKcm",a);
    assert(length(s)==65536 && !memcmp(data(s),data(s)+32768,32768) && !data(s)[65536]);
    run("D1Ev",a);
    memcpy(input,"abcdef",7);a[1]=(uint32_t)(uintptr_t)input;a[2]=6;run("6__initEPKcm",a);
    a[1]=2;a[2]=(uint32_t)(uintptr_t)(data(s)+1);a[3]=3;run("6insertEmPKcm",a);
    assert(!strcmp(data(s),"abbcdcdef"));
    a[1]=1;a[2]=2;run("5eraseEmm",a);assert(!strcmp(data(s),"acdcdef"));
    run("D1Ev",a);
    lp32_fast_import_fn destroy=libcpp_bridge32_fast_import(
        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev");
    assert(destroy && !libcpp_bridge32_fast_import("_not_a_string_destructor"));
    memcpy(input,"destructor test string",23);
    a[1]=(uint32_t)(uintptr_t)input;a[2]=22;run("6__initEPKcm",a);
    assert(destroy(a,0)==a[0] && !s[0] && !s[1] && !s[2]);
    /* Each direct handler matches the chained implementation, including
       growth past the inline buffer and the value compare returns. */
    {
        static const char *const fast_methods[]={"7reserveEm","6appendEPKcm","6appendEPKc",
            "9push_backEc","C1ERKS5_","C2ERKS5_","6__initEPKcm","6__initEPKcmm","6assignEPKc","aSERKS5_"};
        uint32_t *left=s+160,*right=s+164,*source=s+168;
        char *text=(char *)(s+176);memcpy(text,"a fairly long source string",28);
        uint32_t init[]={(uint32_t)(uintptr_t)source,(uint32_t)(uintptr_t)text,27,0};run("6__initEPKcm",init);
        for(unsigned i=0;i<sizeof(fast_methods)/sizeof(fast_methods[0]);++i){
            char name[256];snprintf(name,sizeof(name),"%s%s",prefix,fast_methods[i]);
            lp32_fast_import_fn handler=libcpp_bridge32_fast_import(name);assert(handler);
            for(unsigned side=0;side<2;++side){
                uint32_t *target=side?right:left;memset(target,0,12);
                uint32_t start[]={(uint32_t)(uintptr_t)target,(uint32_t)(uintptr_t)"xy",2,0};run("6__initEPKcm",start);
                uint32_t args[]={(uint32_t)(uintptr_t)target,(uint32_t)(uintptr_t)text,20,40};
                if(!strcmp(fast_methods[i],"7reserveEm"))args[1]=40;
                if(!strcmp(fast_methods[i],"9push_backEc"))args[1]='q';
                if(fast_methods[i][0]=='C'||fast_methods[i][0]=='a')args[1]=(uint32_t)(uintptr_t)source;
                if(fast_methods[i][0]=='C'||!strncmp(fast_methods[i],"6__init",7))memset(target,0,12);
                if(side)assert(handler(args,0)==args[0]);else run(fast_methods[i],args);
            }
            assert(length(left)==length(right)&&!memcmp(data(left),data(right),length(left)+1));
            assert((left[0]&1)==(right[0]&1));
        }
        lp32_fast_import_fn compare=libcpp_bridge32_fast_import(
            "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc");
        uint32_t args[]={(uint32_t)(uintptr_t)source,(uint32_t)(uintptr_t)"a fairly"};
        assert(compare && (int32_t)compare(args,0)>0);
        args[1]=(uint32_t)(uintptr_t)"a fairly long source string";assert(compare(args,0)==0);
        args[1]=(uint32_t)(uintptr_t)"b";assert((int32_t)compare(args,0)<0);
        assert(!traps);
    }
    uint32_t *control=s+128,*vtable=s+144;control[0]=0x30000240;control[1]=0;control[2]=1;vtable[2]=1;vtable[4]=2;
    uint32_t shared_args[]={0x30000200};uint64_t result;
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count12__add_sharedEv",shared_args,&result)&&control[1]==1);
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count16__release_sharedEv",shared_args,&result)&&!shared_destroyed);
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count16__release_sharedEv",shared_args,&result)&&shared_destroyed==1&&!weak_destroyed&&control[2]==0);
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count14__release_weakEv",shared_args,&result)&&weak_destroyed==1);
    puts("libc++ shared count PASS (last strong/weak ownership and guest destructors)");
    puts("libc++ string PASS (short/long ABI, alias growth, embedded NUL, copy, resize, reserve, destruction)");
    for(unsigned i=0;i<2;++i){
        a[0]=(uint32_t)(uintptr_t)s;a[1]=i?UINT32_MAX:2;s[3]=0xabcdef12;
        assert(libcpp_bridge32_dispatch("__ZNSt3__19to_stringEm",a,&result)&&result==a[0]);
        assert(!strcmp(data(s),i?"4294967295":"2")&&s[3]==0xabcdef12);
        run("D1Ev",a);
    }
    for(unsigned i=0;i<2;++i){
        struct timespec before,after;clockid_t clock=i?CLOCK_MONOTONIC:CLOCK_REALTIME;
        uint64_t scale=i?1000000000:1000000,divisor=i?1:1000;
        clock_gettime(clock,&before);
        assert(libcpp_bridge32_dispatch(i?"__ZNSt3__16chrono12steady_clock3nowEv":
            "__ZNSt3__16chrono12system_clock3nowEv",a,&result));
        clock_gettime(clock,&after);
        assert(result>=(uint64_t)before.tv_sec*scale+before.tv_nsec/divisor);
        assert(result<=(uint64_t)after.tv_sec*scale+after.tv_nsec/divisor);
    }
    puts("libc++ clock and unsigned-long formatting PASS (units and i386 width)");
    prefix="__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE";
    wchar_t *wide=(wchar_t *)input;wide[0]=0x10348;wide[1]=0;wide[2]=0xe9;
    a[0]=(uint32_t)(uintptr_t)s;a[1]=(uint32_t)(uintptr_t)wide;a[2]=3;
    run("6__initEPKwm",a);assert(length(s)==3 && s[0]&1);
    assert(!memcmp((void *)(uintptr_t)s[2],wide,12));
    a[1]=8;a[2]=0x10349;run("6resizeEmw",a);
    assert(length(s)==8);wchar_t *value=(void *)(uintptr_t)s[2];
    assert(value[0]==0x10348 && !value[1] && value[2]==0xe9 && value[7]==0x10349 && !value[8]);
    a[1]=(uint32_t)(uintptr_t)wide;run("6assignEPKw",a);
    assert(length(s)==1 && *(wchar_t *)(uintptr_t)s[2]==0x10348);
    a[2]=3;run("6assignEPKwm",a);assert(length(s)==3);
    run("D1Ev",a);
    puts("libc++ wide string PASS (four-byte characters, embedded NUL and resize fill)");
    return 0;
}
