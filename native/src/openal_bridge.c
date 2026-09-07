#include "openal_bridge.h"
#include "compat_runtime.h"
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Host contexts/devices never fit into guest pointer slots. AL buffer/source
 * names and sample data have the same widths in both ABIs. */
static struct { void *pointer; uint32_t token; bool context; } objects[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_token = 0x7f140000;
static uint32_t token(void *p, bool context) {
    if (!p) return 0;
    pthread_mutex_lock(&lock);
    unsigned empty = 1024;
    for (unsigned i=0;i<1024;++i) {
        if (objects[i].pointer==p && objects[i].context==context) {
            uint32_t t=objects[i].token; pthread_mutex_unlock(&lock); return t;
        }
        if (!objects[i].pointer && empty==1024) empty=i;
    }
    uint32_t t=0;
    if(empty<1024) { t=next_token++; objects[empty].pointer=p; objects[empty].token=t; objects[empty].context=context; }
    pthread_mutex_unlock(&lock); return t;
}
static void *pointer(uint32_t t, bool context, bool remove) {
    void *p=NULL; pthread_mutex_lock(&lock);
    for(unsigned i=0;i<1024;++i) if(objects[i].token==t && objects[i].context==context) {
        p=objects[i].pointer; if(remove) memset(&objects[i],0,sizeof(objects[i])); break;
    }
    pthread_mutex_unlock(&lock); return p;
}
static bool muted(void) { const char *v=getenv("LP32_MUTE_AUDIO"); return v && strcmp(v,"0"); }
int openal_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
#define IS(n) (!strcmp(name,n))
#define P(i) ((void *)(uintptr_t)a[i])
    *out=0;
    if(IS("_alcOpenDevice")) *out=token(alcOpenDevice(P(0)),false);
    else if(IS("_alcCloseDevice")) {
        ALCdevice *d=pointer(a[0],false,false); *out=alcCloseDevice(d);
        if(*out) pointer(a[0],false,true);
    } else if(IS("_alcCreateContext")) *out=token(alcCreateContext(pointer(a[0],false,false),P(1)),true);
    else if(IS("_alcDestroyContext")) alcDestroyContext(pointer(a[0],true,true));
    else if(IS("_alcGetCurrentContext")) *out=token(alcGetCurrentContext(),true);
    else if(IS("_alcMakeContextCurrent")) {
        *out=alcMakeContextCurrent(pointer(a[0],true,false));
        if(*out && a[0] && muted()) alListenerf(AL_GAIN,0);
    } else if(IS("_alcGetEnumValue")) *out=alcGetEnumValue(pointer(a[0],false,false),P(1));
    else if(IS("_alcIsExtensionPresent")) *out=alcIsExtensionPresent(pointer(a[0],false,false),P(1));
    else if(IS("_alIsExtensionPresent")) *out=alIsExtensionPresent(P(0));
    else if(IS("_alcGetProcAddress") || IS("_alGetProcAddress")) {
        const char *s=P(IS("_alcGetProcAddress")?1:0);
        if(s && !strcmp(s,"alBufferDataStatic")) *out=compat_runtime32_guest_callback("_alBufferDataStatic");
    } else if(IS("_alGenBuffers")) alGenBuffers((int)a[0],P(1));
    else if(IS("_alGenSources")) alGenSources((int)a[0],P(1));
    else if(IS("_alDeleteBuffers")) alDeleteBuffers((int)a[0],P(1));
    else if(IS("_alDeleteSources")) alDeleteSources((int)a[0],P(1));
    else if(IS("_alBufferData") || IS("_alBufferDataStatic")) alBufferData(a[0],a[1],P(2),a[3],a[4]);
    else if(IS("_alDistanceModel")) alDistanceModel(a[0]);
    else if(IS("_alGetError")) *out=alGetError();
    else if(IS("_alGetFloat")) *out=compat_runtime32_return_double(alGetFloat(a[0]));
    else if(IS("_alGetSourcei")) alGetSourcei(a[0],a[1],P(2));
    else if(IS("_alListenerfv")) {
        float zero=0; alListenerfv(a[0],muted() && a[0]==AL_GAIN ? &zero : P(1));
    } else if(IS("_alSourcePlay")) { if(muted())alListenerf(AL_GAIN,0); alSourcePlay(a[0]); }
    else if(IS("_alSourceStop")) alSourceStop(a[0]);
    else if(IS("_alSourceQueueBuffers")) alSourceQueueBuffers(a[0],a[1],P(2));
    else if(IS("_alSourceUnqueueBuffers")) alSourceUnqueueBuffers(a[0],a[1],P(2));
    else if(IS("_alSourcei")) alSourcei(a[0],a[1],a[2]);
    else if(IS("_alSourcefv")) alSourcefv(a[0],a[1],P(2));
    else if(IS("_alSourcef")) { float f;memcpy(&f,a+2,4);alSourcef(a[0],a[1],f); }
    else return 0;
    return 1;
}
