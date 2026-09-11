#include "audio_converter_bridge.h"
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t next=0x10002000, expected_converter;
static unsigned resample_calls;
uint32_t compat_runtime32_allocate(size_t size,int zero){uint32_t p=next;next+=(size+15)&~15u;assert(next<0x10010000);if(zero)memset((void *)(uintptr_t)p,0,size);return p;}
void compat_runtime32_deallocate(uint32_t p){next=p;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t count){
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
int main(void){
    assert(mmap((void *)0x10000000,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==(void *)0x10000000);
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
    puts("Audio converter resampling PASS (16,384 variable-size fills, four rates, reset, sample values and guards)");
    puts("Audio converter PASS (PCM samples, legacy and complex callback ABI, packed output buffers, reset, disposal)");
}
