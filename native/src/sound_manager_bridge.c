#include "sound_manager_bridge.h"
#include "compat_runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bink's Mac sound backend predates Core Audio. Carbon Sound Manager is
   unavailable to a 64-bit process; retain its i386 channel/command ABI and
   play its decoded PCM through AudioQueue. In particular, callBackCmd must
   run AFTER preceding buffers finish, or Bink overwrites audio still playing.
   Layouts: Apple's Mac OS X 10.6 CarbonSound/Sound.h (two-byte packing). */
#pragma pack(push, 2)
struct command32 { uint16_t cmd; int16_t param1; uint32_t param2; };
struct channel32 {
    uint32_t next, modifier, callback, info, wait;
    struct command32 current;
    int16_t flags, length, head, tail;
    struct command32 commands[128];
};
struct header32 {
    uint32_t samples, channels, rate, loop_start, loop_end;
    uint8_t encoding, base_frequency;
    uint32_t frames;
    uint8_t extended_rate[10];
    uint32_t marker, instruments, aes;
    uint16_t bits, reserved1;
    uint32_t reserved2, reserved3, reserved4;
};
#pragma pack(pop)
_Static_assert(sizeof(struct command32) == 8, "i386 SndCommand");
_Static_assert(sizeof(struct channel32) == 1060, "i386 SndChannel");
_Static_assert(sizeof(struct header32) == 64 && offsetof(struct header32, frames) == 22,
               "i386 ExtSoundHeader");

struct sound_item {
    struct command32 command;
    AudioQueueBufferRef buffer;
    bool done;
    struct sound_item *next;
};
struct sound_channel {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t worker;
    bool used, closing, resetting, paused, callback_active, owns_guest;
    uint32_t guest, callback_command, volume;
    AudioQueueRef queue;
    AudioStreamBasicDescription format;
    struct sound_item *head, *tail;
    unsigned pending;
    uint64_t buffers, callbacks;
};
static struct sound_channel channels[32];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool trace, mute;

static void initialize(void)
{
    trace = getenv("LP32_TRACE_AUDIO") != NULL;
    mute = getenv("LP32_MUTE_AUDIO") != NULL;
    for (unsigned i = 0; i < 32; ++i) {
        pthread_mutex_init(&channels[i].lock, NULL);
        pthread_cond_init(&channels[i].changed, NULL);
    }
}

/* Return with the channel locked; slots and their synchronization objects
   live for the process lifetime, including across dispose/recreate cycles. */
static struct sound_channel *find_channel(uint32_t guest)
{
    for (unsigned i = 0; guest && i < 32; ++i) {
        struct sound_channel *c = &channels[i];
        pthread_mutex_lock(&c->lock);
        if (c->used && !c->closing && c->guest == guest) return c;
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

static void buffer_finished(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    (void)queue;
    struct sound_channel *c = context;
    pthread_mutex_lock(&c->lock);
    struct sound_item *item = buffer->mUserData;
    if (item) item->done = true;
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->lock);
}

static void release_item(struct sound_channel *c, struct sound_item *item)
{
    if (item->buffer) AudioQueueFreeBuffer(c->queue, item->buffer);
    free(item);
}

static void *sound_worker(void *context)
{
    struct sound_channel *c = context;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        if (c->closing) break;
        struct sound_item *item = c->head;
        if (c->resetting || c->paused || !item || (item->buffer && !item->done)) {
            pthread_cond_wait(&c->changed, &c->lock);
            continue;
        }
        c->head = item->next;
        if (!c->head) c->tail = NULL;
        --c->pending;
        pthread_cond_broadcast(&c->changed);
        if (item->command.cmd == 13) {
            uint32_t function = ((struct channel32 *)(uintptr_t)c->guest)->callback;
            memcpy((void *)(uintptr_t)c->callback_command, &item->command, sizeof(item->command));
            uint32_t args[] = {c->guest, c->callback_command};
            c->callback_active = true;
            pthread_mutex_unlock(&c->lock);
            if (function) compat_runtime32_call(function, args, 2);
            pthread_mutex_lock(&c->lock);
            c->callback_active = false;
            ++c->callbacks;
            pthread_cond_broadcast(&c->changed);
        }
        release_item(c, item);
    }
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

static OSStatus set_volume(struct sound_channel *c)
{
    if (!c->queue) return noErr;
    float left = (c->volume & 65535) / 256.0f, right = (c->volume >> 16) / 256.0f;
    float gain = left > right ? left : right;
    OSStatus status = AudioQueueSetParameter(c->queue, kAudioQueueParam_Volume, mute ? 0 : gain);
    if (!status && c->format.mChannelsPerFrame == 2) status = AudioQueueSetParameter(c->queue, kAudioQueueParam_Pan,
        gain ? (right - left) / gain : 0);
    return status;
}

static OSStatus prepare_buffer(struct sound_channel *c, struct sound_item *item)
{
    if (!item->command.param2) return -50;
    const struct header32 *h = (void *)(uintptr_t)item->command.param2;
    unsigned channels_count, bits, frames, offset;
    if (h->encoding == 0) {
        channels_count = 1; bits = 8; frames = h->channels; offset = 22;
    } else if (h->encoding == 255) {
        channels_count = h->channels; bits = h->bits; frames = h->frames; offset = sizeof(*h);
    } else return -223; /* siUnknownInfoType: compressed headers need a codec. */
    if ((channels_count != 1 && channels_count != 2) || (bits != 8 && bits != 16) ||
        h->rate < (1000u << 16) || !frames || frames > 4 * 1024 * 1024) return -50;
    unsigned bytes_per_frame = channels_count * (bits / 8);
    AudioStreamBasicDescription format = {h->rate / 65536.0, kAudioFormatLinearPCM,
        kAudioFormatFlagIsPacked | (bits == 16 ? kAudioFormatFlagIsSignedInteger : 0),
        bytes_per_frame, 1, bytes_per_frame, channels_count, bits, 0};
    if (!c->queue) {
        OSStatus status = AudioQueueNewOutput(&format, buffer_finished, c, NULL, NULL, 0, &c->queue);
        if (status) return status;
        c->format = format;
        status = set_volume(c);
        if (status) return status;
        if (trace) fprintf(stderr, "compat32: Sound Manager PCM %.2f Hz %u-channel %u-bit\n",
                           format.mSampleRate, channels_count, bits);
    } else if (memcmp(&format, &c->format, sizeof(format))) return -50;
    unsigned bytes = frames * bytes_per_frame;
    OSStatus status = AudioQueueAllocateBuffer(c->queue, bytes, &item->buffer);
    if (status) return status;
    const void *data = h->samples ? (void *)(uintptr_t)h->samples : (const uint8_t *)h + offset;
    memcpy(item->buffer->mAudioData, data, bytes);
    item->buffer->mAudioDataByteSize = bytes;
    item->buffer->mUserData = item;
    return noErr;
}

/* Cancel pending commands and stop playback before releasing native buffers.
   AudioQueueReset can synchronously invoke buffer_finished, so never call it
   with the lock held. The worker stays parked until the reset is complete. */
static OSStatus reset_channel(struct sound_channel *c)
{
    if (pthread_equal(pthread_self(), c->worker)) return -50;
    c->resetting = true;
    while (c->callback_active) pthread_cond_wait(&c->changed, &c->lock);
    pthread_mutex_unlock(&c->lock);
    OSStatus status = c->queue ? AudioQueueReset(c->queue) : noErr;
    pthread_mutex_lock(&c->lock);
    if (!status) {
        while (c->head) {
            struct sound_item *item = c->head;
            c->head = item->next;
            release_item(c, item);
        }
        c->tail = NULL; c->pending = 0;
    }
    c->resetting = false;
    pthread_cond_broadcast(&c->changed);
    return status;
}

int sound_manager_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (!strcmp(name, "_NewSndCallBackUPP")) { *result = a[0]; return 1; }
    if (!strcmp(name, "_DisposeSndCallBackUPP")) { *result = 0; return 1; }
    if (strcmp(name, "_SndNewChannel") && strcmp(name, "_SndDisposeChannel") &&
        strcmp(name, "_SndDoCommand") && strcmp(name, "_SndDoImmediate") &&
        strcmp(name, "_SndChannelStatus")) return 0;
    pthread_once(&once, initialize);
    OSStatus status = -50;
    struct sound_channel *c = NULL;
    if (!strcmp(name, "_SndNewChannel")) {
        if (!a[0] || (int16_t)a[1] != 5 || (a[2] & 0xff00)) goto done;
        for (unsigned i = 0; i < 32; ++i) {
            pthread_mutex_lock(&channels[i].lock);
            if (!channels[i].used) { c = &channels[i]; break; }
            pthread_mutex_unlock(&channels[i].lock);
        }
        if (!c) { status = -108; goto done; }
        uint32_t *output = (void *)(uintptr_t)a[0];
        c->owns_guest = !*output;
        c->guest = *output ? *output : compat_runtime32_allocate(sizeof(struct channel32), 1);
        c->callback_command = compat_runtime32_allocate(sizeof(struct command32), 1);
        if (!c->guest || !c->callback_command) status = -108;
        else {
            struct channel32 *g = (void *)(uintptr_t)c->guest;
            g->callback = a[3]; g->length = 128;
            c->volume = 0x01000100; c->used = true;
            status = pthread_create(&c->worker, NULL, sound_worker, c) ? -108 : noErr;
        }
        if (!status) *output = c->guest;
        else {
            if (c->owns_guest) compat_runtime32_deallocate(c->guest);
            compat_runtime32_deallocate(c->callback_command);
            c->used = false; c->guest = c->callback_command = 0;
        }
        goto done;
    }
    c = find_channel(a[0]);
    if (!c) { status = -205; goto done; } /* badChannel */
    if (c->resetting && pthread_equal(pthread_self(), c->worker)) goto done;
    while (c->resetting && !c->closing) pthread_cond_wait(&c->changed, &c->lock);
    if (c->closing) { status = -205; goto done; }
    if (!strcmp(name, "_SndChannelStatus")) {
        if (!a[2] || (int16_t)a[1] < 24) goto done;
        uint8_t sc[24] = {0};
        sc[12] = c->pending != 0 || c->callback_active;
        sc[14] = c->paused;
        memcpy((void *)(uintptr_t)a[2], sc, sizeof(sc));
        status = noErr;
    } else if (!strcmp(name, "_SndDisposeChannel")) {
        if (pthread_equal(pthread_self(), c->worker)) goto done;
        if (!a[1]) {
            if (c->paused) { c->paused = false; if (c->queue) AudioQueueStart(c->queue, NULL); }
            pthread_cond_broadcast(&c->changed);
            while (c->pending || c->callback_active) pthread_cond_wait(&c->changed, &c->lock);
        }
        c->closing = true;
        pthread_cond_broadcast(&c->changed);
        pthread_mutex_unlock(&c->lock);
        pthread_join(c->worker, NULL);
        status = c->queue ? AudioQueueDispose(c->queue, true) : noErr;
        pthread_mutex_lock(&c->lock);
        if (!status) {
            while (c->head) { struct sound_item *item = c->head; c->head = item->next; free(item); }
            if (trace) fprintf(stderr, "compat32: Sound Manager closed: %llu buffers, %llu callbacks\n",
                (unsigned long long)c->buffers, (unsigned long long)c->callbacks);
            if (c->owns_guest) compat_runtime32_deallocate(c->guest);
            compat_runtime32_deallocate(c->callback_command);
            c->used = c->closing = c->paused = false;
            c->guest = c->callback_command = c->pending = 0;
            c->queue = NULL; c->tail = NULL; c->buffers = c->callbacks = 0;
        }
    } else {
        if (!a[1]) goto done;
        struct command32 cmd;
        memcpy(&cmd, (void *)(uintptr_t)a[1], sizeof(cmd));
        bool immediate = !strcmp(name, "_SndDoImmediate");
        if (immediate) {
            switch (cmd.cmd) {
            case 0: status = noErr; break;
            case 3: case 4: status = reset_channel(c); break;
            case 11: c->paused = true; status = c->queue ? AudioQueuePause(c->queue) : noErr; break;
            case 12:
                c->paused = false;
                status = c->queue ? AudioQueueStart(c->queue, NULL) : noErr;
                pthread_cond_broadcast(&c->changed); break;
            case 46: c->volume = cmd.param2; status = set_volume(c); break;
            default: break;
            }
        } else if (cmd.cmd == 81 || cmd.cmd == 13 || cmd.cmd == 0) {
            while (c->pending >= 128 && !a[2] && !c->closing && !c->resetting &&
                   !pthread_equal(pthread_self(), c->worker)) pthread_cond_wait(&c->changed, &c->lock);
            if (c->closing || c->resetting) goto done;
            if (c->pending >= 128) { status = -203; goto done; } /* queueFull */
            struct sound_item *item = calloc(1, sizeof(*item));
            if (!item) { status = -108; goto done; }
            item->command = cmd;
            status = cmd.cmd == 81 ? prepare_buffer(c, item) : noErr;
            if (!status && item->buffer) status = AudioQueueEnqueueBuffer(c->queue, item->buffer, 0, NULL);
            if (status) { release_item(c, item); goto done; }
            if (c->tail) c->tail->next = item; else c->head = item;
            c->tail = item; ++c->pending;
            if (item->buffer) {
                ++c->buffers;
                if (!c->paused) status = AudioQueueStart(c->queue, NULL);
            }
            pthread_cond_broadcast(&c->changed);
        }
        if (status && trace) fprintf(stderr, "compat32: Sound Manager command %u immediate=%d error=%d\n",
                                     cmd.cmd, immediate, (int)status);
    }
done:
    if (c) pthread_mutex_unlock(&c->lock);
    *result = (uint32_t)status;
    return 1;
}
