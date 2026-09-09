#ifndef LP32_TFU_MOVIE_END_H
#define LP32_TFU_MOVIE_END_H
#include <stdint.h>
#include <string.h>

/* TFU's movie render loop calls NextFrame after DoFrame has already
   marked EOF. NextFrame then wraps FrameNum to zero, hiding completion from
   the non-looping movie manager in the caller. Only guard this caller: loading
   loops and explicit seeks must retain the original transport semantics.
   At this call site EBX is the decoder and EDI is the movie manager. */
enum { TFU_MOVIE_END_STUB = 0x7f00f000 };
static const uint8_t tfu_movie_end_completion[] = {
    0x8b,0x7d,0x08,0x80,0x7f,0x0d,0x00,0x75,0x12,
    0x8b,0x43,0x0c,0x3b,0x43,0x08,0x72,0x0a
};
static inline void tfu_movie_end_code_at(uint8_t code[28], uint8_t hook[8],
                                         uint32_t hook_address, uint32_t next_frame)
{
    const uint8_t body[28] = {
        0x80,0x7f,0x0d,0x00,    /* cmp byte [edi+0xd],0 (loop flag) */
        0x75,0x08,              /* jne advance */
        0x8b,0x43,0x0c,         /* mov eax,[ebx+0xc] (FrameNum) */
        0x3b,0x43,0x08,         /* cmp eax,[ebx+8] (Frames) */
        0x73,0x08,              /* jae continue without advancing */
        0x89,0x1c,0x24,         /* advance: mov [esp],ebx */
        0xe8,0,0,0,0,           /* call original NextFrame */
        0xe9,0,0,0,0,           /* continue: jmp past original call */
        0x90
    };
    memcpy(code, body, sizeof(body));
    int32_t relative = (int32_t)(next_frame - (TFU_MOVIE_END_STUB + 22u));
    memcpy(code + 18, &relative, 4);
    relative = (int32_t)((hook_address + 8u) - (TFU_MOVIE_END_STUB + 27u));
    memcpy(code + 23, &relative, 4);
    memset(hook, 0x90, 8);
    hook[0] = 0xe9;
    relative = (int32_t)(TFU_MOVIE_END_STUB - (hook_address + 5u));
    memcpy(hook + 1, &relative, 4);
}
#endif
