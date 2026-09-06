#include <stdint.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 2)
struct command { uint16_t cmd; int16_t param1; uint32_t param2; };
struct channel {
    uint32_t next, modifier, callback, info, wait;
    struct command current;
    int16_t flags, length, head, tail;
    struct command commands[128];
};
struct header {
    uint32_t samples, channels, rate, start, end;
    uint8_t encoding, frequency;
    uint32_t frames;
    uint8_t extended_rate[10];
    uint32_t marker, instrument, aes;
    uint16_t bits, reserved1;
    uint32_t reserved2, reserved3, reserved4;
};
#pragma pack(pop)
extern void *NewSndCallBackUPP(void (*)(struct channel *, struct command *));
extern void DisposeSndCallBackUPP(void *);
extern int16_t SndNewChannel(struct channel **, int16_t, int32_t, void *);
extern int16_t SndDisposeChannel(struct channel *, uint8_t);
extern int16_t SndDoCommand(struct channel *, const struct command *, uint8_t);
extern int16_t SndDoImmediate(struct channel *, const struct command *);
extern int16_t SndChannelStatus(struct channel *, int16_t, void *);
extern int32_t OTAtomicAdd32(int32_t, int32_t *);

static int32_t callbacks;
static struct channel *expected;
static void finished(struct channel *channel, struct command *cmd)
{
    if (channel != expected || channel->info != 0x12345678 || cmd->cmd != 13 ||
        cmd->param1 != 789 || cmd->param2 != (uint32_t)(callbacks + 1)) callbacks = -100;
    else OTAtomicAdd32(1, &callbacks);
}
static int immediate(struct channel *channel, uint16_t code)
{
    struct command cmd = {code, 0, 0};
    return SndDoImmediate(channel, &cmd);
}

int check_sound_manager(void)
{
    callbacks = 0;
    int32_t atomic = 8;
    if (OTAtomicAdd32(-3, &atomic) != 5 || atomic != 5) return -230;
    void *upp = NewSndCallBackUPP(finished);
    struct { struct channel *value; uint32_t guard; } output = {0, 0xabcdef01};
    if (!upp || SndNewChannel(&output.value, 5, 0xc0, upp) ||
        !output.value || output.guard != 0xabcdef01) return -231;
    expected = output.value;
    expected->info = 0x12345678;
    /* Pause before buffering, then resume: callbacks must not run early or
       receive high native pointers, and each queued command is a value copy. */
    if (immediate(expected, 11)) return -232;
    int16_t pcm[4410 * 2] = {0};
    struct header h = {0};
    h.samples = (uintptr_t)pcm; h.channels = 2; h.rate = 44100u << 16;
    h.encoding = 255; h.frames = 4410; h.bits = 16;
    for (int i = 1; i <= 3; ++i) {
        struct command cmd = {81, 0, (uintptr_t)&h};
        if (SndDoCommand(expected, &cmd, 0)) return -233;
        cmd = (struct command){13, 789, i};
        if (SndDoCommand(expected, &cmd, 0)) return -234;
        memset(&cmd, 0xa5, sizeof(cmd));
    }
    usleep(150000);
    struct { uint8_t status[24]; uint32_t guard; } sc = {{0}, 0xabcdef01};
    if (callbacks || SndChannelStatus(expected, 24, sc.status) ||
        !sc.status[12] || !sc.status[14] || sc.guard != 0xabcdef01) return -235;
    if (immediate(expected, 12)) return -236;
    for (int i = 0; i < 300 && callbacks >= 0 && callbacks < 3; ++i) usleep(10000);
    if (callbacks != 3 || SndChannelStatus(expected, 24, sc.status) || sc.status[12]) return -237;
    if (immediate(expected, 65535) != -50) return -238;
    /* Flushing a paused queue must cancel its callbacks without a use-after-
       free when Core Audio returns buffers during reset/disposal. */
    if (immediate(expected, 11)) return -239;
    struct command cmd = {81, 0, (uintptr_t)&h};
    if (SndDoCommand(expected, &cmd, 0)) return -240;
    cmd = (struct command){13, 789, 4};
    if (SndDoCommand(expected, &cmd, 0) || immediate(expected, 4) || immediate(expected, 3) ||
        immediate(expected, 12)) return -241;
    usleep(150000);
    if (callbacks != 3 || SndDisposeChannel(expected, 1)) return -242;
    if (SndChannelStatus(expected, 24, sc.status) != -205) return -243;
    /* Reuse more than the host slot count; no worker or channel leaks. */
    for (int i = 0; i < 40; ++i) {
        output.value = 0;
        if (SndNewChannel(&output.value, 5, 0x80, upp) || SndDisposeChannel(output.value, 1)) return -244;
    }
    DisposeSndCallBackUPP(upp);
    return 0;
}
