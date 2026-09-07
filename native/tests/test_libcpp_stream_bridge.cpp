#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/mman.h>
#include "../src/libcpp_stream_bridge.cpp"
static uint32_t cursor=0x10000000;
static std::map<uint32_t,std::string> callbacks;
extern "C" uint32_t compat_runtime32_allocate(size_t n,int clear) { uint32_t p=cursor;cursor+=(n+15)&~15u;if(clear)memset(w(p),0,n);return p; }
extern "C" void compat_runtime32_deallocate(uint32_t) {}
extern "C" uint32_t compat_runtime32_guest_callback(const char *name) {uint32_t p=0x20000000+(uint32_t)callbacks.size()*4;callbacks[p]=name;return p;}
extern "C" uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t) {uint64_t result;assert(callbacks.count(fn));assert(libcpp_stream_bridge32_dispatch(callbacks[fn].c_str(),a,&result));return (uint32_t)result;}
extern "C" int compat_runtime32_pointer_import_matches(uint32_t p,const char *name) {return p==0x30000000&&!strcmp(name,"__ZNSt3__17codecvtIcc11__mbstate_tE2idE");}
static uint64_t call(const char *name,std::initializer_list<uint32_t> a) {uint64_t out;assert(libcpp_stream_bridge32_dispatch(name,a.begin(),&out));return out;}
int main() {
    assert(mmap((void *)0x10000000,1<<20,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0)==(void *)0x10000000);
    uint32_t storage=compat_runtime32_allocate(88,1),b=compat_runtime32_allocate(36,1),vt=compat_runtime32_allocate(20,1);
    w(storage)[0]=vt+12;w(vt)[0]=8; // istream -> virtual basic_ios offset.
    uint32_t g=storage+8;w(g)[18]=0x12345678;w(b)[8]=0x87654321;
    call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev",{b});
    call("__ZNSt3__18ios_base4initEPv",{g,b});
    assert(w(g)[18]==0x12345678&&w(b)[8]==0x87654321&&w(g)[2]==6&&w(g)[4]==0);
    uint32_t text=compat_runtime32_allocate(64,1);strcpy((char *)w(text),"42 900");
    w(b)[2]=w(b)[3]=text;w(b)[4]=text+6;
    uint32_t value=compat_runtime32_allocate(8,1);w(value)[1]=0xfeedface;
    call("__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEErsERj",{storage,value});assert(*w(value)==42&&w(value)[1]==0xfeedface);
    call("__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEErsERm",{storage,value});assert(*w(value)==900&&w(value)[1]==0xfeedface);
    assert(w(g)[4]&std::ios::eofbit);
    call("__ZNSt3__18ios_base5clearEj",{g,0});
    w(b)[5]=w(b)[6]=text;w(b)[7]=text+64;
    call("__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEj",{storage,123});
    assert(w(b)[6]==text+3&&!memcmp(w(text),"123",3));
    uint32_t loc=compat_runtime32_allocate(8,1);w(loc)[1]=0xaabbccdd;
    call("__ZNKSt3__18ios_base6getlocEv",{loc,g});assert(w(loc)[1]==0xaabbccdd);
    uint32_t facet=call("__ZNKSt3__16locale9use_facetERNS0_2idE",{loc,0x30000000});
    assert(invoke(facet,7)==1&&invoke(facet,6)==1);
    call("__ZNSt3__16localeD1Ev",{loc});
    call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev",{b});
    call("__ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev",{g});
    puts("libc++ streams PASS (i386 layouts, guest buffers, parsing, output, locale, codecvt)");
}
