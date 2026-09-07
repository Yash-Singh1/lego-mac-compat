#define dlsym test_dlsym
#include "../src/steam_bridge.c"
#include <assert.h>
#include <sys/mman.h>
static bool modern;
void *test_dlsym(void *library,const char *name){(void)library;return modern&&!strcmp(name,"SteamAPI_ISteamRemoteStorage_FileReadAsync")?(void *)1:NULL;}
int steam_storage_fix_apply(const struct mach_header_64 *image){(void)image;return 0;}
uint32_t compat_runtime32_allocate(size_t size,int clear){static uint32_t next=0x10001000;uint32_t p=next;next+=(uint32_t)(size+15)&~15u;if(clear)memset((void *)(uintptr_t)p,0,size);return p;}
void compat_runtime32_deallocate(uint32_t p){(void)p;}
uint32_t compat_runtime32_copy_cstring(const char *s){if(!s)return 0;uint32_t p=compat_runtime32_allocate(strlen(s)+1,0);strcpy((void *)(uintptr_t)p,s);return p;}
uint32_t compat_runtime32_guest_callback(const char *name){(void)name;return 0;}
static unsigned callback_calls;
static unsigned char callback_payload[20];
static uint32_t callback_args[5];
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t n){
    assert(fn == 0x1234 && (n == 2 || n == 5));
    assert(a[0] == 0x10000200 && a[1]);
    memcpy(callback_payload, (void *)(uintptr_t)a[1], sizeof(callback_payload));
    memcpy(callback_args, a, n * sizeof(*a));
    ++callback_calls;
    return 0;
}
static uint64_t user_id(void *self){assert(self==interfaces[STEAM_USER].host);return UINT64_C(0x1122334455667788);}
static unsigned forget_calls;
static bool forget(void *self,const char *name){assert(self==interfaces[STEAM_STORAGE].host&&!strcmp(name,"slot"));++forget_calls;return true;}
static bool quota(void *self,int32_t *total,int32_t *available){assert(self==interfaces[STEAM_STORAGE].host);*total=200;*available=100;return true;}
static bool hooked;
static void hook(void *self,bool enabled){assert(self==interfaces[STEAM_SCREENSHOTS].host);hooked=enabled;}
static bool stats_success = true;
static unsigned request_calls, set_calls, store_calls;
static bool request_stats(void *self){assert(self==interfaces[STEAM_STATS].host);++request_calls;return stats_success;}
static bool set_achievement(void *self,const char *name){assert(self==interfaces[STEAM_STATS].host&&!strcmp(name,"slot"));++set_calls;return stats_success;}
static bool store_stats(void *self){assert(self==interfaces[STEAM_STATS].host);++store_calls;return stats_success;}
int main(void){
    const char *previous_id = getenv("SteamAppId");
    char *saved_id = previous_id ? strdup(previous_id) : NULL;
    unsetenv("SteamAppId");
    assert(!steam_bridge32_prepare_environment(0) && !getenv("SteamAppId"));
    assert(!steam_bridge32_prepare_environment(249130));
    assert(!strcmp(getenv("SteamAppId"), "249130"));
    assert(!steam_bridge32_prepare_environment(249130));
    assert(steam_bridge32_prepare_environment(32440) == -1);
    assert(!strcmp(getenv("SteamAppId"), "249130"));
    setenv("SteamAppId", "", 1);
    assert(!steam_bridge32_prepare_environment(249130));
    if (saved_id) { setenv("SteamAppId", saved_id, 1); free(saved_id); }
    else unsetenv("SteamAppId");
    uint32_t *memory=mmap((void *)0x10000000,65536,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);assert(memory==(void *)0x10000000);
    void *user_table[64]={0},**user=user_table;user_table[2]=user_id;
    interfaces[STEAM_USER].host=&user;interfaces[STEAM_USER].guest=0x10000000;memory[0]=0x12345678;
    uint32_t a[]={0x10000000,0,0,0,0};uint64_t out;
    assert(interface_call(STEAM_USER,2,a,&out)&&out==UINT64_C(0x1122334455667788));assert(memory[0]==0x12345678);
    assert(!steam_bridge32_call_uses_sret("_lp32_steam_0_2",a));
    a[0]=0x10000010;a[1]=0x10000000;memory[6]=0xabcdef01;
    assert(steam_bridge32_call_uses_sret("_lp32_steam_0_2",a));
    assert(interface_call(STEAM_USER,2,a,&out)&&out==0x10000010);assert(*(uint64_t *)(memory+4)==UINT64_C(0x1122334455667788)&&memory[6]==0xabcdef01);
    void *storage_table[64]={0},**storage=storage_table;interfaces[STEAM_STORAGE].host=&storage;steam_library=(void *)1;
    strcpy((char *)memory+64,"slot");a[1]=0x10000040;
    for(unsigned version=0;version<2;++version){modern=version;memset(storage_table,0,sizeof(storage_table));unsigned f=modern?5:2,q=modern?20:17;
        storage_table[f]=forget;storage_table[q]=quota;
        a[1]=0x10000040;assert(interface_call(STEAM_STORAGE,f,a,&out)&&out==1);
        a[1]=0x10000080;a[2]=0x10000088;memory[33]=0xabcdef01;memory[35]=0xabcdef01;
        assert(interface_call(STEAM_STORAGE,q,a,&out)&&out==1&&memory[32]==200&&memory[34]==100&&memory[33]==0xabcdef01&&memory[35]==0xabcdef01);
    }
    assert(forget_calls==2);
    void *screen_table[64]={0},**screen=screen_table;screen_table[3]=hook;interfaces[STEAM_SCREENSHOTS].host=&screen;a[1]=1;
    assert(interface_call(STEAM_SCREENSHOTS,3,a,&out)&&hooked);
    void *stats_table[16]={0}, **stats=stats_table;
    stats_table[0]=request_stats;stats_table[7]=set_achievement;stats_table[10]=store_stats;
    interfaces[STEAM_STATS].host=&stats;
    a[1]=0x10000040;
    for(unsigned success=0;success<2;++success){
        stats_success=success;
        assert(steam_bridge32_dispatch("_lp32_steam_1_0",a,&out)&&out==success);
        assert(steam_bridge32_dispatch("_lp32_steam_1_7",a,&out)&&out==success);
        assert(steam_bridge32_dispatch("_lp32_steam_1_10",a,&out)&&out==success);
    }
    assert(request_calls==2&&set_calls==2&&store_calls==2);
    /* Four-byte-packed UserStatsReceived_t: game ID, result, user ID.
       Check callback marshalling independently of the real Steam service. */
    memory[128]=0x10000210;memory[132]=0x1234;memory[133]=0x1234;
    const uint32_t payload[]={249130,0,1,0x55667788,0x11223344};
    struct steam_callback cb={.guest=0x10000200,.id=1101,.size=20};
    callback_run(&cb,(void *)payload);
    assert(callback_calls==1&&!memcmp(callback_payload,payload,sizeof(payload)));
    callback_result(&cb,(void *)payload,true,UINT64_C(0xabcdef0123456789));
    assert(callback_calls==2&&!memcmp(callback_payload,payload,sizeof(payload)));
    assert(callback_args[2]==1&&callback_args[3]==0x23456789&&callback_args[4]==0xabcdef01);
    puts("Steam bridge PASS (app identity, SteamID ABI, storage, achievements, callback payloads)");
}
