#include "audio_converter_bridge.h"
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
static _Thread_local uint32_t next=0x10002000;
static uint32_t expected_converter;
static unsigned resample_calls;
static pthread_mutex_t barrier_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_changed=PTHREAD_COND_INITIALIZER;
static bool callback_entered, callback_release;
static unsigned disposals_finished;
static int32_t call(const char *,uint32_t *);
uint32_t compat_runtime32_allocate(size_t size,int zero){uint32_t p=next;next+=(size+15)&~15u;assert(next<0x10040000);if(zero)memset((void *)(uintptr_t)p,0,size);return p;}
void compat_runtime32_deallocate(uint32_t p){next=p;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t count){
    if(fn==10 || fn==11 || fn==12){
        assert(count==5);
        uint32_t token=a[0];
        if(fn==10){
            assert(call("_AudioConverterReset",&token)==kAudio_ParamError);
            pthread_mutex_lock(&barrier_lock);
            callback_entered=true;pthread_cond_broadcast(&barrier_changed);
            while(!callback_release)pthread_cond_wait(&barrier_changed,&barrier_lock);
            pthread_mutex_unlock(&barrier_lock);
        } else if(fn==11){
            assert(!call("_AudioConverterDispose",&token));
            assert(call("_AudioConverterDispose",&token)==kAudio_ParamError);
            assert(call("_AudioConverterReset",&token)==kAudio_ParamError);
        }
        uint32_t *packets=(void *)(uintptr_t)a[1],*list=(void *)(uintptr_t)a[2];
        assert(*packets>=4 && list[0]==1);
        *packets=4;list[1]=2;list[2]=16;list[3]=0x10000800;
        if(a[3])*(uint32_t *)(uintptr_t)a[3]=0;
        return 0;
    }
    if(fn==9){
        assert(count==5 && a[0]==expected_converter && a[4]==123);
        uint32_t *packets=(void *)(uintptr_t)a[1],*list=(void *)(uintptr_t)a[2];
        /* Bound the test's deliberately broken baseline: a stuck native SRC
           keeps requesting one input packet without completing this fill. */
        if(++resample_calls>128){*packets=0;return (uint32_t)kAudio_ParamError;}
        assert(*packets<=1024 && list[0]==2);
        for(unsigned i=0;i<2;++i){list[1+i*3]=1;list[2+i*3]=*packets*2;list[3+i*3]=0x10000800;}
        if(a[3])*(uint32_t *)(uintptr_t)a[3]=0;
        return 0;
    }
    if(fn==8){assert(count==4 && a[0]==expected_converter && a[3]==123);*(uint32_t *)(uintptr_t)a[1]=16;*(uint32_t *)(uintptr_t)a[2]=0x10000800;return 0;}
    assert(fn==7 && count==5 && a[0]==expected_converter && a[4]==123);
    uint32_t *packets=(void *)(uintptr_t)a[1],*list=(void *)(uintptr_t)a[2];
    assert(*packets>=4 && list[0]==1);
    *packets=4;list[0]=1;list[1]=2;list[2]=16;list[3]=0x10000800;
    if(a[3])*(uint32_t *)(uintptr_t)a[3]=0;
    return 0;
}
static int32_t call(const char *name,uint32_t *a){uint64_t out;assert(audio_converter_bridge32_dispatch(name,a,&out));return (int32_t)out;}
struct job { const char *name; uint32_t args[6],scratch; int32_t status; bool begun,done; };
static void *run_job(void *raw){
    struct job *job=raw;next=job->scratch;
    pthread_mutex_lock(&barrier_lock);job->begun=true;pthread_cond_broadcast(&barrier_changed);pthread_mutex_unlock(&barrier_lock);
    job->status=call(job->name,job->args);
    pthread_mutex_lock(&barrier_lock);job->done=true;
    if(!strcmp(job->name,"_AudioConverterDispose"))++disposals_finished;
    pthread_cond_broadcast(&barrier_changed);pthread_mutex_unlock(&barrier_lock);return NULL;
}
/* Every expected transition has a deadline, so a lifecycle deadlock fails the
   CPU-only test instead of hanging its runner. */
static void wait_flag(const bool *flag){
    struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
    pthread_mutex_lock(&barrier_lock);
    while(!*flag)assert(!pthread_cond_timedwait(&barrier_changed,&barrier_lock,&deadline));
    pthread_mutex_unlock(&barrier_lock);
}
static struct job fill_job(uint32_t token,uint32_t callback,uint32_t base,uint32_t scratch){
    *(uint32_t *)(uintptr_t)base=4;
    uint32_t *list=(void *)(uintptr_t)(base+0x100);
    list[0]=1;list[1]=2;list[2]=32;list[3]=base+0x200;
    return (struct job){.name="_AudioConverterFillComplexBuffer",.args={token,callback,123,base,base+0x100,0},.scratch=scratch};
}
static void check_lifetime(uint32_t *create){
    AudioStreamBasicDescription *source=(void *)0x10000000,*destination=(void *)0x10000040;
    *source=(AudioStreamBasicDescription){44100,kAudioFormatLinearPCM,kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,4,1,4,2,16,0};
    *destination=(AudioStreamBasicDescription){44100,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,8,1,8,2,32,0};
    assert(!call("_AudioConverterNew",create));uint32_t first=*(uint32_t *)0x10000100;
    assert(!call("_AudioConverterNew",create));uint32_t second=*(uint32_t *)0x10000100;
    struct job fill=fill_job(first,10,0x10010000,0x10020000);
    pthread_t fill_thread;assert(!pthread_create(&fill_thread,NULL,run_job,&fill));wait_flag(&callback_entered);
    struct job reset={.name="_AudioConverterReset",.args={first},.scratch=0x10024000};
    pthread_t reset_thread;assert(!pthread_create(&reset_thread,NULL,run_job,&reset));wait_flag(&reset.begun);
    /* Another converter must progress while the first one's native input
       callback is blocked. This rejects a registry-wide native-call lock. */
    struct job independent=fill_job(second,12,0x10011000,0x10028000);
    pthread_t independent_thread;assert(!pthread_create(&independent_thread,NULL,run_job,&independent));wait_flag(&independent.done);
    assert(!pthread_join(independent_thread,NULL));assert(!independent.status);
    assert(!call("_AudioConverterReset",&second));assert(!call("_AudioConverterDispose",&second));
    struct job dispose[2]={
        {.name="_AudioConverterDispose",.args={first},.scratch=0x10024000},
        {.name="_AudioConverterDispose",.args={first},.scratch=0x10028000}};
    pthread_t dispose_thread[2];
    for(unsigned i=0;i<2;++i)assert(!pthread_create(&dispose_thread[i],NULL,run_job,&dispose[i]));
    struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
    pthread_mutex_lock(&barrier_lock);
    while(!disposals_finished)assert(!pthread_cond_timedwait(&barrier_changed,&barrier_lock,&deadline));
    /* Exactly one caller is rejected. The actual disposer is still waiting
       for the live Fill, and the callback has not been released. */
    assert(disposals_finished==1);
    assert(dispose[0].done!=dispose[1].done);
    assert((dispose[0].done?dispose[0].status:dispose[1].status)==kAudio_ParamError);
    pthread_mutex_unlock(&barrier_lock);
    wait_flag(&reset.done);assert(reset.status==kAudio_ParamError);
    pthread_mutex_lock(&barrier_lock);callback_release=true;pthread_cond_broadcast(&barrier_changed);pthread_mutex_unlock(&barrier_lock);
    wait_flag(&fill.done);assert(!pthread_join(fill_thread,NULL));assert(!fill.status);
    for(unsigned i=0;i<2;++i){wait_flag(&dispose[i].done);assert(!pthread_join(dispose_thread[i],NULL));}
    assert(!pthread_join(reset_thread,NULL));
    assert((dispose[0].status==noErr && dispose[1].status==kAudio_ParamError) ||
           (dispose[1].status==noErr && dispose[0].status==kAudio_ParamError));
    assert(call("_AudioConverterReset",&first)==kAudio_ParamError);
    /* Callback self-disposal retires the handle immediately and frees the
       native converter only after Fill and its callback have returned. */
    assert(!call("_AudioConverterNew",create));uint32_t token=*(uint32_t *)0x10000100;
    struct job reentrant=fill_job(token,11,0x10010000,0x10020000);
    assert(!call(reentrant.name,reentrant.args));
    assert(call("_AudioConverterReset",&token)==kAudio_ParamError);
    assert(call("_AudioConverterDispose",&token)==kAudio_ParamError);
    puts("Audio converter lifetime PASS (blocked fill, reset, duplicate disposal, independent converter, callback self-disposal)");
}
int main(void){
    assert(mmap((void *)0x10000000,0x40000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==(void *)0x10000000);
    AudioStreamBasicDescription *source=(void *)0x10000000,*destination=(void *)0x10000040;
    *source=(AudioStreamBasicDescription){44100,kAudioFormatLinearPCM,kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,4,1,4,2,16,0};
    *destination=(AudioStreamBasicDescription){44100,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,8,1,8,2,32,0};
    *(uint32_t *)0x10000104=0x12345678;
    uint32_t create[]={0x10000000,0x10000040,0x10000100};assert(!call("_AudioConverterNew",create));
    expected_converter=*(uint32_t *)0x10000100;assert(expected_converter && *(uint32_t *)0x10000104==0x12345678);
    const int16_t pcm[]={-32768,32767,0,16384,-16384,0,8192,-8192};memcpy((void *)0x10000800,pcm,sizeof(pcm));
    uint32_t *packets=(void *)0x10000200;packets[0]=4;packets[1]=0xaabbccdd;
    uint32_t *buffers=(void *)0x10000300;buffers[0]=1;buffers[1]=2;buffers[2]=32;buffers[3]=0x10000900;buffers[4]=0xdeadbeef;
    uint32_t fill[]={expected_converter,7,123,0x10000200,0x10000300,0};
    assert(!call("_AudioConverterFillComplexBuffer",fill));
    assert(packets[0]==4 && packets[1]==0xaabbccdd && buffers[2]==32 && buffers[4]==0xdeadbeef);
    float *converted=(void *)0x10000900;
    for(unsigned i=0;i<8;++i)assert(fabsf(converted[i]-pcm[i]/32768.0f)<0.00001f);
    assert(!call("_AudioConverterReset",&expected_converter));
    packets[0]=32;memset((void *)0x10000900,0,32);
    uint32_t legacy[]={expected_converter,8,123,0x10000200,0x10000900};
    assert(!call("_AudioConverterFillBuffer",legacy));
    assert(packets[0]==32 && packets[1]==0xaabbccdd);
    for(unsigned i=0;i<8;++i)assert(fabsf(converted[i]-pcm[i]/32768.0f)<0.00001f);
    assert(!call("_AudioConverterDispose",&expected_converter));
    assert(call("_AudioConverterReset",&expected_converter)==kAudio_ParamError);
    /* Exact frozen-game formats: duplicate planar int16 input at 13,544 Hz,
       mono float output at 44.1 kHz, alternating 470/471-frame callbacks.
       The default macOS SRC loops on the second fill. Also cover ordinary
       game rates, repeated reset, buffer guards, and non-silent DC output. */
    const double rates[]={13544,11025,22050,48000};
    for(unsigned rate=0;rate<sizeof(rates)/sizeof(rates[0]);++rate){
        *source=(AudioStreamBasicDescription){rates[rate],kAudioFormatLinearPCM,44,2,1,2,2,16,0};
        *destination=(AudioStreamBasicDescription){44100,kAudioFormatLinearPCM,9,4,1,4,1,32,0};
        assert(!call("_AudioConverterNew",create));expected_converter=*(uint32_t *)0x10000100;
        for(unsigned i=0;i<1024;++i)((int16_t *)0x10000800)[i]=8192;
        for(unsigned iteration=0;iteration<4096;++iteration){
            if(iteration==2048)assert(!call("_AudioConverterReset",&expected_converter));
            unsigned frames=470+(iteration%2);packets[0]=frames;
            buffers[0]=1;buffers[1]=1;buffers[2]=frames*4;buffers[3]=0x10001000;
            *(uint32_t *)(uintptr_t)(0x10001000+frames*4)=0x9876abcd;
            fill[0]=expected_converter;fill[1]=9;resample_calls=0;
            int32_t status=call("_AudioConverterFillComplexBuffer",fill);
            if(status || packets[0]!=frames || resample_calls>128){
                fprintf(stderr,"SRC regression: rate=%g iteration=%u status=%d packets=%u/%u callbacks=%u\n",rates[rate],iteration,status,packets[0],frames,resample_calls);
                return 1;
            }
            assert(buffers[2]==frames*4 && packets[1]==0xaabbccdd && buffers[4]==0xdeadbeef);
            assert(*(uint32_t *)(uintptr_t)(0x10001000+frames*4)==0x9876abcd);
            /* Allow the native minimum-phase filter's roughly 0.09 dB DC
               attenuation after settling, while detecting silence/corruption. */
            if(iteration%2048>4)
                for(unsigned i=0;i<frames;++i){
                    float value=((float *)0x10001000)[i];
                    if(!isfinite(value) || fabsf(value-0.25f)>=0.003f){
                        fprintf(stderr,"SRC sample regression: rate=%g iteration=%u sample=%u value=%g\n",rates[rate],iteration,i,value);
                        return 1;
                    }
                }
        }
        assert(!call("_AudioConverterDispose",&expected_converter));
    }
    check_lifetime(create);
    puts("Audio converter resampling PASS (16,384 variable-size fills, four rates, reset, sample values and guards)");
    puts("Audio converter PASS (PCM samples, legacy and complex callback ABI, packed output buffers, reset, disposal)");
}
