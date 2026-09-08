#import <Foundation/Foundation.h>
#import <OpenAL/al.h>
#import <OpenAL/alc.h>
#include "openal_bridge.h"
#include "objc_bridge.h"
#include "compat_runtime.h"
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static void *pointer(uint32_t value) { return [(NSValue *)objc_bridge32_host_object(value) pointerValue]; }
static uint32_t handle(void *value) { return value ? objc_bridge32_guest_object([NSValue valueWithPointer:value]) : 0; }
static float real(uint32_t bits) { float value; memcpy(&value, &bits, 4); return value; }
static bool muted(void) { return getenv("LP32_MUTE_AUDIO") || getenv("LP32_HEADLESS"); }
static uint32_t copy_string(const char *text, bool list) {
    if (!text) return 0;
    size_t size = strlen(text) + 1;
    if (list) { while (text[size]) size += strlen(text + size) + 1; ++size; }
    static NSMutableDictionary *strings;
    static NSLock *lock;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ strings = [[NSMutableDictionary alloc] init]; lock = [[NSLock alloc] init]; });
    NSData *key = [NSData dataWithBytes:text length:size];
    [lock lock]; uint32_t result = [strings[key] unsignedIntValue];
    if (!result && size <= UINT32_MAX) {
        result = compat_runtime32_allocate((uint32_t)size, 0);
        if (result) { memcpy((void *)(uintptr_t)result, text, size); strings[key] = @(result); }
    }
    [lock unlock]; return result;
}

int openal_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (strncmp(name, "_al", 3)) return 0;
    @autoreleasepool {
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (IS("alcOpenDevice")) RETURN(handle(alcOpenDevice(PTR(0))));
    if (IS("alcCloseDevice")) RETURN(alcCloseDevice(pointer(a[0])));
    if (IS("alcCreateContext")) RETURN(handle(alcCreateContext(pointer(a[0]), PTR(1))));
    if (IS("alcDestroyContext")) { alcDestroyContext(pointer(a[0])); RETURN(0); }
    if (IS("alcMakeContextCurrent")) {
        ALCboolean ok = alcMakeContextCurrent(pointer(a[0]));
        if (ok && a[0] && muted()) alListenerf(AL_GAIN, 0);
        RETURN(ok);
    }
    if (IS("alcProcessContext")) { alcProcessContext(pointer(a[0])); RETURN(0); }
    if (IS("alcSuspendContext")) { alcSuspendContext(pointer(a[0])); RETURN(0); }
    if (IS("alcGetError")) RETURN(alcGetError(pointer(a[0])));
    if (IS("alcGetString")) RETURN(copy_string(alcGetString(pointer(a[0]), a[1]), !a[0] && a[1] == ALC_DEVICE_SPECIFIER && alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT")));
    if (IS("alGetError")) RETURN(alGetError());
    if (IS("alGetString")) RETURN(copy_string(alGetString(a[0]), false));
    if (IS("alGenBuffers")) { alGenBuffers(a[0], PTR(1)); RETURN(0); }
    if (IS("alDeleteBuffers")) { alDeleteBuffers(a[0], PTR(1)); RETURN(0); }
    if (IS("alGenSources")) { alGenSources(a[0], PTR(1)); RETURN(0); }
    if (IS("alDeleteSources")) { alDeleteSources(a[0], PTR(1)); RETURN(0); }
    if (IS("alBufferData")) { alBufferData(a[0], a[1], PTR(2), a[3], a[4]); RETURN(0); }
    if (IS("alSourceQueueBuffers")) { alSourceQueueBuffers(a[0], a[1], PTR(2)); RETURN(0); }
    if (IS("alSourceUnqueueBuffers")) { alSourceUnqueueBuffers(a[0], a[1], PTR(2)); RETURN(0); }
    if (IS("alSourcePlay")) { alSourcePlay(a[0]); RETURN(0); }
    if (IS("alSourcePause")) { alSourcePause(a[0]); RETURN(0); }
    if (IS("alGetSourcef")) { alGetSourcef(a[0], a[1], PTR(2)); RETURN(0); }
    if (IS("alGetSourcei")) { alGetSourcei(a[0], a[1], PTR(2)); RETURN(0); }
    if (IS("alSourcef")) { alSourcef(a[0], a[1], real(a[2])); RETURN(0); }
    if (IS("alSourcei")) { alSourcei(a[0], a[1], a[2]); RETURN(0); }
    if (IS("alSource3f")) { alSource3f(a[0], a[1], real(a[2]), real(a[3]), real(a[4])); RETURN(0); }
    if (IS("alListener3f")) { alListener3f(a[0], real(a[1]), real(a[2]), real(a[3])); RETURN(0); }
    if (IS("alListenerf")) { alListenerf(a[0], a[0] == AL_GAIN && muted() ? 0 : real(a[1])); RETURN(0); }
    if (IS("alListenerfv")) {
        if (a[0] == AL_GAIN && muted()) alListenerf(AL_GAIN, 0);
        else alListenerfv(a[0], PTR(1));
        RETURN(0);
    }
    if (IS("alDistanceModel")) { alDistanceModel(a[0]); RETURN(0); }
    if (IS("alDopplerFactor")) { alDopplerFactor(real(a[0])); RETURN(0); }
    if (IS("alDopplerVelocity")) { alDopplerVelocity(real(a[0])); RETURN(0); }
    if (IS("alSpeedOfSound")) { alSpeedOfSound(real(a[0])); RETURN(0); }
    }
    return 0;
#undef IS
#undef PTR
#undef RETURN
}

int openal_bridge32_self_test(void) {
    @autoreleasepool {
        uint32_t memory = compat_runtime32_allocate(4096, 1);
        if (!memory) return -1;
        uint32_t a[8] = {0}; uint64_t r = 0;
#define CALL(s) openal_bridge32_dispatch("_" s, a, &r)
        CALL("alcOpenDevice"); uint32_t device = r;
        if (!device) { compat_runtime32_deallocate(memory); return -1; }
        a[0] = device; a[1] = 0; CALL("alcCreateContext"); uint32_t context = r;
        int failed = !context;
        if (context) {
            a[0] = context; CALL("alcMakeContextCurrent"); failed |= !r;
            float listener_gain = .75f, actual_gain = -1;
            a[0] = AL_GAIN; memcpy(a + 1, &listener_gain, 4); CALL("alListenerf");
            alGetListenerf(AL_GAIN, &actual_gain); failed |= actual_gain != (muted() ? 0 : listener_gain);
            memcpy((void *)(uintptr_t)(memory + 16), &listener_gain, 4);
            a[1] = memory + 16; CALL("alListenerfv");
            alGetListenerf(AL_GAIN, &actual_gain); failed |= actual_gain != (muted() ? 0 : listener_gain);
            a[0] = 1; a[1] = memory; CALL("alGenBuffers"); uint32_t buffer = *(uint32_t *)(uintptr_t)memory;
            a[1] = memory + 4; CALL("alGenSources"); uint32_t source = *(uint32_t *)(uintptr_t)(memory + 4);
            a[0] = buffer; a[1] = AL_FORMAT_MONO16; a[2] = memory + 32; a[3] = 1024; a[4] = 22050; CALL("alBufferData");
            a[0] = source; a[1] = AL_BUFFER; a[2] = buffer; CALL("alSourcei");
            a[2] = memory + 8; CALL("alGetSourcei"); failed |= *(uint32_t *)(uintptr_t)(memory + 8) != buffer;
            float gain = .375f; a[1] = AL_GAIN; memcpy(a + 2, &gain, 4); CALL("alSourcef");
            a[2] = memory + 12; CALL("alGetSourcef"); failed |= *(float *)(uintptr_t)(memory + 12) != gain;
            CALL("alGetError"); failed |= r != AL_NO_ERROR;
            a[0] = 1; a[1] = memory + 4; CALL("alDeleteSources");
            a[1] = memory; CALL("alDeleteBuffers");
            a[0] = 0; CALL("alcMakeContextCurrent"); a[0] = context; CALL("alcDestroyContext");
        }
        a[0] = device; CALL("alcCloseDevice"); failed |= !r;
        compat_runtime32_deallocate(memory);
        fprintf(stderr, "OpenAL self-test: %s buffer upload, source attachment, float parameters, cleanup (no playback)\n", failed ? "FAIL" : "PASS");
        return failed ? -1 : 0;
#undef CALL
    }
}
