#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <ctype.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <locale.h>
#include <iconv.h>
#include <wchar.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

struct guest_string { uint32_t words[3]; };
extern struct guest_string cpp_to_string(unsigned long) __asm__("__ZNSt3__19to_stringEm");
extern void cpp_string_destroy(struct guest_string *) __asm__("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev");
extern int64_t system_now(void) __asm__("__ZNSt3__16chrono12system_clock3nowEv");
extern int64_t steady_now(void) __asm__("__ZNSt3__16chrono12steady_clock3nowEv");
extern void cpp_notify_all(pthread_cond_t *) __asm__("__ZNSt3__118condition_variable10notify_allEv");
extern void cpp_notify_one(pthread_cond_t *) __asm__("__ZNSt3__118condition_variable10notify_oneEv");
struct guest_lock { pthread_mutex_t *mutex; unsigned char owns; };
extern void cpp_timed_wait(pthread_cond_t *, struct guest_lock *, int64_t)
    __asm__("__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILx1ELx1000000000EEEEEEE");
static int scan(const char *text,const char *format,...) {
    va_list args;va_start(args,format);int n=vsscanf(text,format,args);va_end(args);return n;
}

extern int dependency_value(void);
extern int MW2FutureTest(void);
extern _Bool OSAtomicCompareAndSwap64Barrier(int64_t,int64_t,volatile int64_t *);
static int initialized;
__attribute__((constructor)) static void initialize(void) { ++initialized; }
static __attribute__((noinline)) void unwind(jmp_buf state, int value, int depth) {
    volatile uint32_t frame[32];
    frame[depth] = 0x12345678;
    if (depth) unwind(state, value, depth - 1);
    longjmp(state, value + (frame[depth] != 0x12345678));
}
static void *tls_probe(void *raw) {
    pthread_key_t key = *(pthread_key_t *)raw;
    if (pthread_getspecific(key)) return (void *)1;
    if (pthread_setspecific(key, (void *)0xabc)) return (void *)2;
    for (int i=0;i<4;++i) if (pthread_getspecific(key)!=(void *)0xabc) return (void *)3;
    return 0;
}
int FixtureMain(void) {
    if (dependency_value() != 22 || initialized != 1) return -1;
    struct { jmp_buf state; uint32_t guard; } saved;
    saved.guard = 0xabc123;
    volatile int count = 0;
    int value = setjmp(saved.state);
    if (!value) { ++count; unwind(saved.state, 0, 10); }
    if (value != 1 || count != 1 || saved.guard != 0xabc123) return -2;
    value = setjmp(saved.state);
    if (!value) unwind(saved.state, 42, 6);
    if (value != 42) return -3;
    sigset_t original, blocked = 1u << (SIGUSR1 - 1), current;
    if (sigprocmask(SIG_BLOCK, &blocked, &original)) return -4;
    sigjmp_buf signal_state;
    value = sigsetjmp(signal_state, 1);
    if (!value) {
        sigprocmask(SIG_UNBLOCK, &blocked, 0);
        siglongjmp(signal_state, 7);
    }
    sigprocmask(SIG_SETMASK, 0, &current);
    sigprocmask(SIG_SETMASK, &original, 0);
    if (value != 7 || !(current & blocked)) return -5;
    volatile double rate = 59.94, negative = -1.5;
    volatile float fraction = 0.5f;
    if (lround(rate) != 60 || lround(negative) != -2 || lroundf(-2.5f) != -3) return -6;
    if (asinf(fraction) < 0.5235f || asinf(fraction) > 0.5237f ||
        acosf(fraction) < 1.0471f || acosf(fraction) > 1.0473f || atof("-1.25") != -1.25) return -7;
    struct { struct rlimit limit; uint32_t guard; } resources = {{0}, 0x12345678};
    if (getrlimit(RLIMIT_NOFILE, &resources.limit) || resources.guard != 0x12345678 ||
        resources.limit.rlim_cur < 16 || setrlimit(RLIMIT_NOFILE, &resources.limit)) return -8;
    struct { char path[4096]; uint32_t guard; } path = {{0}, 0x98765432};
    if (getcwd(path.path, sizeof(path.path)) != path.path || path.guard != 0x98765432 || path.path[0] != '/') return -9;
    char *allocated = getcwd(NULL, 0);
    if (!allocated || allocated[0] != '/') return -10;
    free(allocated);
    /* Exercise imports from guest code, not just direct host loader APIs.
       PunkBuster opens a module, resolves entry points and calls them in i386. */
    void *module = dlopen("bin/libfixture_dep.dylib", RTLD_NOW);
    int (*get_value)(void) = module ? dlsym(module, "dependency_value") : 0;
    if (!get_value || get_value() != 22) return -11;
    if (dlsym(module, "missing_entry") || !dlerror() || dlerror()) return -12;
    if (dlopen("bin/broken.dylib", RTLD_NOW) || !dlerror() || dlerror()) return -13;
    /* Exact non-ASCII byte path taken by LAN_CompareHostname, plus an ASCII
       mask with a nonzero result so an unsupported-import zero cannot pass. */
    if (__maskrune('A', _CTYPE_A) != _CTYPE_A ||
        __maskrune(0xc3, _CTYPE_A) || __maskrune(-1, _CTYPE_A)) return -14;
    if (dlclose(module)) return -15;
    module = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW);
    int (*parse_number)(const char *) = module ? dlsym(module, "atoi") : 0;
    if (!parse_number || parse_number("12345") != 12345 || dlclose(module)) return -16;
    /* The first import resolves a handler; later calls use the cached path.
       Check guest pointer returns, signed comparisons, padding and guards. */
    pthread_t thread = pthread_self();
    for (unsigned pass = 0; pass < 3; ++pass) {
        struct { char bytes[40]; uint32_t guard; } destination;
        destination.guard = 0x1234abcd;
        const char *text = "Materials/SHIP/metal";
        if (strcpy(destination.bytes, text) != destination.bytes ||
            strlen(destination.bytes) != 20 || strcmp(destination.bytes, text)) return -17;
        if (strcmp("\xff", "\x7f") <= 0 || strcmp("a", "ab") >= 0 ||
            memcmp("a\0b", "a\0c", 3) >= 0 || memcmp("a", "b", 0)) return -18;
        for (unsigned n = 0; n <= sizeof(destination.bytes); ++n) {
            memset(destination.bytes, 0x55, sizeof(destination.bytes));
            if (strncpy(destination.bytes, "ship", n) != destination.bytes) return -19;
            for (unsigned i = 0; i < sizeof(destination.bytes); ++i) {
                char expected = i >= n ? 0x55 : i < 4 ? "ship"[i] : 0;
                if (destination.bytes[i] != expected) return -20;
            }
        }
        if (destination.guard != 0x1234abcd || !thread || pthread_self() != thread) return -21;
        if (!setlocale(LC_CTYPE, "C")) return -73;
        for (int c = -1; c <= 256; ++c) {
            int expected = c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
            if (__tolower(c) != expected) return -22;
        }
    }
    /* Exercise direct i386 imports and dlsym at a protected page boundary. */
    char *bounded=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(bounded==MAP_FAILED || mprotect(bounded+4096,4096,PROT_NONE))return -70;
    bounded[4094]='z';bounded[4095]=0;
    int (*dynamic_compare)(const char *,const char *)=dlsym(RTLD_DEFAULT,"strcmp");
    if(!dynamic_compare || strcmp(bounded+4095,"") || strcmp(bounded+4094,"z") ||
       dynamic_compare(bounded+4094,"z") || dynamic_compare("\x80","\xff")>=0)return -71;
    munmap(bounded,8192);
    pthread_key_t key;pthread_t worker;void *worker_result=NULL;
    if(pthread_key_create(&key,NULL) || pthread_setspecific(key,(void *)0x123) ||
       pthread_create(&worker,NULL,tls_probe,&key) || pthread_join(worker,&worker_result) ||
       worker_result || pthread_getspecific(key)!=(void *)0x123)return -72;
    /* MW2's SDL startup uses all three calls. The timed wait takes an i386
       timespec, and signal/cancellation output pointers are four bytes. */
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    struct { struct timespec deadline; uint32_t guard[2]; } timeout = {{0, 0}, {0xabcdef12, 0x76543210}};
    if (pthread_mutex_lock(&mutex)) return -23;
    if (pthread_cond_timedwait(&condition, &mutex, &timeout.deadline) != ETIMEDOUT) return -24;
    if (timeout.guard[0] != 0xabcdef12 || timeout.guard[1] != 0x76543210) return -25;
    if (pthread_mutex_unlock(&mutex) || pthread_cond_destroy(&condition) || pthread_mutex_destroy(&mutex)) return -26;
    struct { sigset_t mask; uint32_t guard; } old_mask = {0, 0x1234abcd};
    if (pthread_sigmask(SIG_BLOCK, &blocked, &old_mask.mask) || old_mask.guard != 0x1234abcd) return -27;
    if (pthread_sigmask(SIG_SETMASK, 0, &current) || !(current & blocked)) return -28;
    if (pthread_sigmask(SIG_SETMASK, &old_mask.mask, 0)) return -29;
    struct { int type; uint32_t guard; } old_type = {0, 0x2345bcde};
    if (pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_type.type) || old_type.guard != 0x2345bcde) return -30;
    if (pthread_setcanceltype(old_type.type, 0)) return -31;
    struct { int number; float fraction; char word[8]; uint32_t guard; } parsed = {0,0,{0},0xabc12345};
    if (scan("42 1.25 test", "%d %f %7s", &parsed.number, &parsed.fraction, parsed.word)!=3 ||
        parsed.number!=42 || parsed.fraction!=1.25f || strcmp(parsed.word,"test") || parsed.guard!=0xabc12345) return -32;
    struct timespec pause={0,1000000};
    if(nanosleep(&pause,0))return -33;
    pause.tv_nsec=1000000000;
    if(nanosleep(&pause,0)!=-1 || errno!=EINVAL)return -34;
    struct { int64_t value; uint32_t guard; } atomic={INT64_C(0x123456789abcdef0),0x3456789a};
    if(OSAtomicCompareAndSwap64Barrier(0,1,&atomic.value) ||
       !OSAtomicCompareAndSwap64Barrier(INT64_C(0x123456789abcdef0),INT64_C(0x234567891abcdef0),&atomic.value) ||
       atomic.value!=INT64_C(0x234567891abcdef0) || atomic.guard!=0x3456789a)return -68;
    /* Preserve adjacent words when widening iconv's char** and size_t*.
       A short output buffer must report E2BIG with partial progress. */
    iconv_t converter=iconv_open("UTF-32LE","UTF-8");
    if(converter==(iconv_t)-1)return -41;
    struct { char *pointer; uint32_t guard; } input={"A\xc3\xa9",0x12345678};
    wchar_t wide[8]={0};
    struct { char *pointer; uint32_t guard; } output={(char *)wide,0x23456789};
    struct { size_t length; uint32_t guard; } in_left={3,0x3456789a},out_left={4,0x456789ab};
    if(iconv(converter,&input.pointer,&in_left.length,&output.pointer,&out_left.length)!=(size_t)-1 ||
       errno!=E2BIG || in_left.length!=2 || out_left.length || output.pointer!=(char *)(wide+1) || wide[0]!=L'A')return -42;
    out_left.length=4;
    if(iconv(converter,&input.pointer,&in_left.length,&output.pointer,&out_left.length) ||
       in_left.length || out_left.length || wide[1]!=0xe9 || output.pointer!=(char *)(wide+2))return -43;
    if(input.guard!=0x12345678 || output.guard!=0x23456789 || in_left.guard!=0x3456789a || out_left.guard!=0x456789ab)return -44;
    struct { int trivial; uint32_t guard; } flag={0,0x87654321};
    if(iconvctl(converter,ICONV_TRIVIALP,&flag.trivial) || flag.trivial || flag.guard!=0x87654321)return -45;
    if(iconv(converter,0,0,0,0) || iconv_close(converter))return -46;
    if(iconv_close(converter)!=-1 || errno!=EBADF)return -47;
    if(iconv_open("LP32-invalid-encoding","UTF-8")!=(iconv_t)-1 || errno!=EINVAL)return -48;
    if(wcslen(wide)!=2 || wcscpy(wide,L"cat")!=wide || wcscat(wide,L"car")!=wide ||
       wcslen(wide)!=6 || wcschr(wide,L'c')!=wide || wcsrchr(wide,L'c')!=wide+3 ||
       wcsstr(wide,L"car")!=wide+3 || wmemcmp(wide,L"cat",3))return -49;
    if(!strcmp(getenv("LP32_GAME"),"mw2")) {
        struct { vm_size_t size; uint32_t guard; } page={0,0x1234abcd};
        if(host_page_size(mach_host_self(),&page.size) || !page.size || page.guard!=0x1234abcd || !vm_page_size)return -61;
        mach_vm_address_t address=0;mach_vm_size_t size=0;
        unsigned regions=0;
        for(;;) {
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=0;
            kern_return_t status=mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object);
            if(status==KERN_INVALID_ADDRESS)break;
            if(status || !size || address>=0x100000000ULL || size>0x100000000ULL-address || ++regions>8192)return -62;
            address+=size;
        }
        if(!regions)return -63;
        unsigned char *mapping=mmap(0,2*page.size,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(mapping==MAP_FAILED || (uintptr_t)mapping%page.size)return -64;
        if(mmap(mapping,page.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=mapping)return -65;
        mapping[0]=17;mapping[page.size-1]=29;
        if(mprotect(mapping,page.size,PROT_READ) || mapping[0]!=17 || mapping[page.size-1]!=29)return -66;
        if(munmap(mapping,2*page.size))return -67;
        /* Clang's hidden string-return pointer is popped by the callee. This
           also exercises the high word of the int64 clock returns. */
        struct { struct guest_string text; uint32_t guard; } formatted;
        formatted.guard=0x4567cdef;formatted.text=cpp_to_string(UINT32_MAX);
        char *text=formatted.text.words[0]&1?(void *)formatted.text.words[2]:(char *)&formatted.text+1;
        if(strcmp(text,"4294967295") || formatted.guard!=0x4567cdef)return -35;
        cpp_string_destroy(&formatted.text);
        int64_t wall=system_now(),start=steady_now();
        pause.tv_nsec=2000000;if(nanosleep(&pause,0))return -36;
        if(wall<1000000000000000LL || steady_now()-start<1000000 || system_now()<wall)return -37;
        mutex=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        condition=(pthread_cond_t)PTHREAD_COND_INITIALIZER;
        struct guest_lock lock={&mutex,1};
        if(pthread_mutex_lock(&mutex))return -38;
        cpp_timed_wait(&condition,&lock,0);
        if(pthread_mutex_unlock(&mutex))return -39;
        cpp_notify_one(&condition);cpp_notify_all(&condition);
        if(pthread_cond_destroy(&condition)||pthread_mutex_destroy(&mutex))return -40;
        int future=MW2FutureTest();if(future)return future;
    }
    return 26;
}
