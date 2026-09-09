#!/usr/bin/env python3
"""Exercise the shipped TFU frame-advance/finish code in i386 emulation.

No player, window, decoder, game process, or GPU context is started. Unicorn
is also used by the repository's image unpacker. The actual C patch emitter
supplies the patch bytes; the original game image supplies the guest code.
"""
from pathlib import Path
import struct
import subprocess
import tempfile

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EIP,
)

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / 'build/TFU-Compat.app/Contents/SharedSupport/TFU.image'
data = IMAGE.read_bytes()
segments = []
offset = 28
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    command, size = struct.unpack_from('<II', data, offset)
    if command == 1:
        va, _, file_offset, file_size = struct.unpack_from('<4I', data, offset + 24)
        segments.append((va, file_offset, file_size))
    offset += size


def read(address, size):
    for base, file_offset, file_size in segments:
        if base <= address and address + size <= base + file_size:
            begin = file_offset + address - base
            return data[begin:begin + size]
    raise ValueError(hex(address))


with tempfile.TemporaryDirectory(prefix='tfu-movie-end-') as tmp:
    src = Path(tmp) / 'emit.c'
    exe = Path(tmp) / 'emit'
    src.write_text('''#include <stdio.h>
#include "tfu_movie_end.h"
int main(void) {
    unsigned char code[28], hook[8];
    tfu_movie_end_code(code, hook);
    fwrite(tfu_movie_end_expected, 1, sizeof(tfu_movie_end_expected), stdout);
    fwrite(tfu_movie_end_completion, 1, sizeof(tfu_movie_end_completion), stdout);
    fwrite(code, 1, sizeof(code), stdout);
    fwrite(hook, 1, sizeof(hook), stdout);
}
''')
    subprocess.run(['cc', '-I', str(ROOT / 'src'), str(src), '-o', str(exe)], check=True)
    emitted = subprocess.check_output([str(exe)])
assert len(emitted) == 61
assert read(0x734831, 8) == emitted[:8], 'frame-advance signature mismatch'
assert read(0x734be2, 17) == emitted[8:25], 'completion signature mismatch'


def run(frame, count, loop, patched):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    for address in [0x9f000, 0x734000, 0x3a94000, 0x3aa3000, 0x3a96000,
                    0x7f00f000, 0x10000000]:
        uc.mem_map(address, 4096)
    uc.mem_write(0x9f722, read(0x9f722, 0x71))  # original NextFrame, including rewind
    uc.mem_write(0x734831, read(0x734831, 8))
    uc.mem_write(0x734be2, read(0x734be2, 27))   # actual non-loop finish check
    if patched:
        uc.mem_write(0x7f00f000, emitted[25:53])
        uc.mem_write(0x734831, emitted[53:])
    decoder, manager, stack = 0x10000000, 0x10000100, 0x10000f00
    uc.mem_write(decoder + 8, struct.pack('<III', count, frame, 123))
    uc.mem_write(manager + 13, bytes([loop]))
    uc.mem_write(stack + 8, struct.pack('<I', manager))  # caller's frame pointer
    for reg, value in [(UC_X86_REG_EBX, decoder), (UC_X86_REG_EDI, manager),
                       (UC_X86_REG_ESI, 0x12345678), (UC_X86_REG_EBP, stack),
                       (UC_X86_REG_ESP, stack - 256)]:
        uc.reg_write(reg, value)
    rewinds = []

    def intercept(uc, address, size, context):
        # Replace ONLY external locking and movie rewind. Frame arithmetic,
        # conditional branches, register saves, and stack cleanup are real.
        if address in [0x3aa3627, 0x3aa3631, 0x3a96eb8]:
            if address == 0x3a96eb8:
                rewinds.append(struct.unpack('<I', uc.mem_read(uc.reg_read(UC_X86_REG_ESP) + 4, 4))[0])
            esp = uc.reg_read(UC_X86_REG_ESP)
            target = struct.unpack('<I', uc.mem_read(esp, 4))[0]
            uc.reg_write(UC_X86_REG_ESP, esp + 4)
            uc.reg_write(UC_X86_REG_EIP, target)
        if address in [0x7342ba, 0x734bfd]:
            uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, intercept)
    uc.emu_start(0x734831, 0x734839, count=100)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x734839
    assert uc.reg_read(UC_X86_REG_ESP) == stack - 256
    assert uc.reg_read(UC_X86_REG_EBX) == decoder
    assert uc.reg_read(UC_X86_REG_EDI) == manager
    assert uc.reg_read(UC_X86_REG_ESI) == 0x12345678
    assert uc.reg_read(UC_X86_REG_EBP) == stack
    next_frame = struct.unpack('<I', uc.mem_read(decoder + 12, 4))[0]
    uc.emu_start(0x734be2, 0, count=25)
    finished = uc.reg_read(UC_X86_REG_EIP) == 0x7342ba
    return next_frame, bool(rewinds), finished


# Reproduce the real race: DoFrame sees EOF and sets FrameNum=Frames after
# the earlier Wait test has returned false. The old NextFrame hides EOF.
assert run(514, 514, 0, False) == (0, True, False)
assert run(514, 514, 0, True) == (514, False, True)
cases = 0
for count in [0, 1, 2, 514, 1164]:
    for frame in sorted({0, max(0, count - 1), count, count + 1}):
        for loop in [0, 1]:
            before = run(frame, count, loop, False)
            after = run(frame, count, loop, True)
            if not loop and frame >= count:
                assert after == (frame, False, True), (frame, count, loop, after)
            else:
                assert after == before, (frame, count, loop, before, after)
            cases += 1
print(f'PASS: reproduced EOF replay; {cases} boundary cases; intentional loops and register/stack preservation')
