#include "libcpp_bridge.h"
#include "compat_runtime.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Apple libc++ ABI v1 i386: a 12-byte string. Verified against the guest's
 * inline accessors: bit 0 selects {capacity+1 | 1, size, pointer}; otherwise
 * byte 0 is size*2 and characters follow at their natural alignment. */
static uint32_t *words(uint32_t p){return (void *)(uintptr_t)p;}
static uint32_t length(uint32_t p){uint32_t *s=words(p);return s[0]&1?s[1]:*(uint8_t *)s>>1;}
static uint32_t data(uint32_t p,unsigned width){return words(p)[0]&1?words(p)[2]:p+width;}
static uint32_t capacity(uint32_t p,unsigned width){return words(p)[0]&1?(words(p)[0]&~1u)-1:11/width-1;}
static void set_length(uint32_t p,uint32_t n){if(words(p)[0]&1)words(p)[1]=n;else *(uint8_t *)words(p)=(uint8_t)(n*2);}
static bool reserve(uint32_t p,uint32_t n,unsigned width){
    if(n<=capacity(p,width))return true;
    if(n>UINT32_MAX/width-2)return false;
    uint32_t cap=(n+2)&~1u, mem=compat_runtime32_allocate((size_t)cap*width,1);
    if(!mem)return false;uint32_t len=length(p);
    memcpy((void *)(uintptr_t)mem,(void *)(uintptr_t)data(p,width),(size_t)(len+1)*width);
    if(words(p)[0]&1)compat_runtime32_deallocate(words(p)[2]);
    words(p)[0]=cap|1;words(p)[1]=len;words(p)[2]=mem;return true;
}
static bool assign(uint32_t p,const void *source,uint32_t n,unsigned width,bool initialize,uint32_t wanted){
    if(n>UINT32_MAX/width-2)return false;
    /* Source may alias the string, including its inline representation. */
    void *copy=malloc((size_t)n*width+1);if(!copy)return false;
    if(n)memcpy(copy,source,(size_t)n*width);
    if(initialize)memset(words(p),0,12);
    if(!reserve(p,wanted>n?wanted:n,width)){free(copy);return false;}
    void *dst=(void *)(uintptr_t)data(p,width);memcpy(dst,copy,(size_t)n*width);
    memset((char *)dst+(size_t)n*width,0,width);set_length(p,n);free(copy);return true;
}
static bool replace(uint32_t p,uint32_t pos,uint32_t removed,const void *source,uint32_t n,unsigned width){
    uint32_t len=length(p);if(pos>len)return false;if(removed>len-pos)removed=len-pos;
    if(n>UINT32_MAX-len+removed)return false;uint32_t total=len-removed+n;
    void *buf=malloc((size_t)total*width+1);if(!buf)return false;
    const char *old=(void *)(uintptr_t)data(p,width);
    memcpy(buf,old,(size_t)pos*width);if(n)memcpy((char *)buf+(size_t)pos*width,source,(size_t)n*width);
    memcpy((char *)buf+(size_t)(pos+n)*width,old+(size_t)(pos+removed)*width,(size_t)(len-pos-removed)*width);
    bool ok=assign(p,buf,total,width,false,total);free(buf);return ok;
}
static int shared_count(const char *name,const uint32_t *a,uint64_t *out) {
    const char *base="__ZNSt3__114__shared_count", *weak="__ZNSt3__119__shared_weak_count";
    bool has_weak=!strncmp(name,weak,strlen(weak));
    if(!has_weak&&strncmp(name,base,strlen(base)))return 0;
    const char *method=name+strlen(has_weak?weak:base);
    uint32_t *object=words(a[0]);*out=0;
    if(!strcmp(method,"D1Ev")||!strcmp(method,"D2Ev"))return 1; /* Empty base destructors. */
    if(!strcmp(method,"12__add_sharedEv")){__atomic_add_fetch((int32_t *)(object+1),1,__ATOMIC_RELAXED);return 1;}
    if(has_weak&&!strcmp(method,"10__add_weakEv")){__atomic_add_fetch((int32_t *)(object+2),1,__ATOMIC_RELAXED);return 1;}
    if(!strcmp(method,"16__release_sharedEv")){
        bool last=__atomic_sub_fetch((int32_t *)(object+1),1,__ATOMIC_ACQ_REL)==-1;
        if(last){uint32_t fn=words(object[0])[2];compat_runtime32_call(fn,a,1);}
        if(last&&has_weak&&__atomic_sub_fetch((int32_t *)(object+2),1,__ATOMIC_ACQ_REL)==-1){
            uint32_t fn=words(object[0])[4];compat_runtime32_call(fn,a,1);
        }
        *out=has_weak?0:last;return 1;
    }
    if(has_weak&&!strcmp(method,"14__release_weakEv")){
        if(__atomic_sub_fetch((int32_t *)(object+2),1,__ATOMIC_ACQ_REL)==-1){uint32_t fn=words(object[0])[4];compat_runtime32_call(fn,a,1);}return 1;
    }
    return 0;
}
int libcpp_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if(!strcmp(name,"__ZNSt3__112__next_primeEm")) {
        uint32_t n=a[0];
        if(n<3){*out=n?2:0;return 1;}
        n|=1;
        for(;;) {
            bool prime=true;
            for(uint32_t d=3;d<=n/d;d+=2)if(n%d==0){prime=false;break;}
            if(prime){*out=n;return 1;}
            if(n>UINT32_MAX-2)return 0;
            n+=2;
        }
    }
    if(shared_count(name,a,out))return 1;
    const char *method=NULL;unsigned width=1;
    const char *prefixes[]={"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE",
        "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE",
        "__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE"};
    for(unsigned i=0;i<3;++i)if(!strncmp(name,prefixes[i],strlen(prefixes[i]))){method=name+strlen(prefixes[i]);width=i==2?4:1;break;}
    if(!method)return 0;
#define IS(s) (!strcmp(method,s))
#define P(i) ((void *)(uintptr_t)a[i])
    uint32_t p=a[0];*out=p;
    if(IS("6__initEPKcm") || IS("6__initEPKcmm"))return assign(p,P(1),a[2],width,true,IS("6__initEPKcmm")?a[3]:a[2]);
    if(IS("C1ERKS5_") || IS("C2ERKS5_"))return assign(p,(void *)(uintptr_t)data(a[1],width),length(a[1]),width,true,0);
    if(IS("C1ERKS5_mmRKS4_") || IS("C2ERKS5_mmRKS4_")){
        uint32_t len=length(a[1]),pos=a[2],n=a[3];
        if(pos>len)return 0; /* Requires guest out_of_range exception support. */
        if(n>len-pos)n=len-pos;
        return assign(p,(const char *)(uintptr_t)data(a[1],width)+(size_t)pos*width,n,width,true,0);
    }
    if(IS("D1Ev") || IS("D2Ev")){if(words(p)[0]&1)compat_runtime32_deallocate(words(p)[2]);memset(words(p),0,12);return 1;}
    if(IS("aSERKS5_"))return assign(p,(void *)(uintptr_t)data(a[1],width),length(a[1]),width,false,0);
    if(IS("6assignEPKc"))return assign(p,P(1),(uint32_t)strlen(P(1)),width,false,0);
    if(IS("7reserveEm"))return reserve(p,a[1],width);
    if(IS("6appendEPKc") || IS("6appendEPKcm"))return replace(p,length(p),0,P(1),IS("6appendEPKc")?(uint32_t)strlen(P(1)):a[2],width);
    if(IS("6insertEmPKc") || IS("6insertEmPKcm"))return replace(p,a[1],0,P(2),IS("6insertEmPKc")?(uint32_t)strlen(P(2)):a[3],width);
    if(IS("5eraseEmm"))return replace(p,a[1],a[2],NULL,0,width);
    if(IS("9push_backEc") || IS("9push_backEw"))return replace(p,length(p),0,a+1,1,width);
    if(IS("6resizeEmc")){
        uint32_t len=length(p),n=a[1];if(n<=len)return replace(p,n,len-n,NULL,0,width);
        if(!reserve(p,n,width))return 0;memset((char *)(uintptr_t)data(p,width)+len,a[2],n-len);
        set_length(p,n);*(char *)(uintptr_t)(data(p,width)+n)=0;return 1;
    }
    if(IS("7compareEPKc") || IS("7compareEmmPKc")){
        uint32_t pos=IS("7compareEPKc")?0:a[1],n=length(p);if(pos>n)return 0;n-=pos;
        if(IS("7compareEmmPKc") && n>a[2])n=a[2];const char *s=IS("7compareEPKc")?P(1):P(3);
        size_t right=strlen(s);int cmp=memcmp((char *)(uintptr_t)data(p,1)+pos,s,n<right?n:right);
        *out=(uint32_t)(cmp?cmp:n<right?-1:n>right?1:0);return 1;
    }
    if(IS("4findEcm") || IS("5rfindEcm")){
        uint32_t len=length(p);const unsigned char *s=(void *)(uintptr_t)data(p,1);*out=UINT32_MAX;
        if(IS("4findEcm")){for(uint32_t i=a[2];i<len;++i)if(s[i]==(uint8_t)a[1]){*out=i;break;}}
        else if(len){uint32_t i=a[2]<len?a[2]:len-1;do{if(s[i]==(uint8_t)a[1]){*out=i;break;}}while(i--);}
        return 1;
    }
    if(IS("9__grow_byEmmmmmm")){
        uint32_t old=length(p),copy=a[4],del=a[5],add=a[6];
        if(copy>old || del>old-copy || a[2]>UINT32_MAX-a[1])return 0;
        uint32_t cap=a[1]+a[2];if(!reserve(p,cap,width))return 0;
        char *d=(void *)(uintptr_t)data(p,width);
        memmove(d+(size_t)(copy+add)*width,d+(size_t)(copy+del)*width,(size_t)(old-copy-del)*width);
        return 1;
    }
    return 0;
}
