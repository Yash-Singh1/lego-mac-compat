#include "openal_capture_bridge.h"
#include <OpenAL/alc.h>
#include <pthread.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Capture is Source's fallback microphone backend. Keep native device pointers
   behind handles, including the open-failure/close(NULL) path. */
enum { CAPTURE_BASE = 0x7f0b0001, CAPTURE_COUNT = 16 };
static ALCdevice *devices[CAPTURE_COUNT];
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static ALCenum bridge_error;

int openal_capture_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (strncmp(name, "_alcCapture", 11) && strcmp(name, "_alcGetError") &&
        strcmp(name, "_alcGetIntegerv")) return 0;
    if (strcmp(name, "_alcCaptureOpenDevice") && strcmp(name, "_alcCaptureCloseDevice") &&
        strcmp(name, "_alcCaptureStart") && strcmp(name, "_alcCaptureStop") &&
        strcmp(name, "_alcCaptureSamples") && strcmp(name, "_alcGetError") &&
        strcmp(name, "_alcGetIntegerv")) return 0;
    *result = 0;
    pthread_mutex_lock(&capture_lock);
    if (!strcmp(name, "_alcCaptureOpenDevice")) {
        if (!a[1] || (int32_t)a[3] <= 0) { bridge_error = ALC_INVALID_VALUE; goto done; }
        unsigned i = 0;
        while (i < CAPTURE_COUNT && devices[i]) ++i;
        if (i == CAPTURE_COUNT) { bridge_error = ALC_OUT_OF_MEMORY; goto done; }
        devices[i] = alcCaptureOpenDevice((const char *)(uintptr_t)a[0], a[1], a[2], (ALCsizei)a[3]);
        if (devices[i]) *result = CAPTURE_BASE + i;
        goto done;
    }
    unsigned i = a[0] - CAPTURE_BASE;
    ALCdevice *device = i < CAPTURE_COUNT ? devices[i] : NULL;
    if (!strcmp(name, "_alcGetError")) {
        *result = bridge_error ? bridge_error : alcGetError(device);
        bridge_error = ALC_NO_ERROR;
    } else if (!device) {
        /* In particular, close after a failed open returns ALC_FALSE. */
        bridge_error = ALC_INVALID_DEVICE;
    } else if (!strcmp(name, "_alcCaptureCloseDevice")) {
        *result = alcCaptureCloseDevice(device);
        if (*result) devices[i] = NULL;
    } else if (!strcmp(name, "_alcCaptureStart")) alcCaptureStart(device);
    else if (!strcmp(name, "_alcCaptureStop")) alcCaptureStop(device);
    else if (!strcmp(name, "_alcCaptureSamples")) {
        if (!a[1] || (int32_t)a[2] < 0) bridge_error = ALC_INVALID_VALUE;
        else alcCaptureSamples(device, (void *)(uintptr_t)a[1], (ALCsizei)a[2]);
    } else {
        if (!a[3] || (int32_t)a[2] <= 0) bridge_error = ALC_INVALID_VALUE;
        else alcGetIntegerv(device, a[1], (ALCsizei)a[2], (ALCint *)(uintptr_t)a[3]);
    }
done:
    pthread_mutex_unlock(&capture_lock);
    return 1;
}
