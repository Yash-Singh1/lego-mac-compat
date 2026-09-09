#!/usr/bin/env python3
"""Exercise the shipped TFU frame-advance/finish code in i386 emulation.

No player, window, decoder, game process, or GPU context is started. Unicorn
is also used by the repository's image unpacker. The actual C patch emitter
supplies the patch bytes; the original game image supplies the guest code.
"""
from pathlib import Path
import ctypes as C
import sys
import re
import struct
import subprocess
import tempfile

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from test_tfu_layout import Layout, code_section

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EIP,
)

ROOT = Path(__file__).resolve().parents[1]
IMAGE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/TFU-Compat.app/Contents/SharedSupport/TFU.image'
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
    libpath = Path(tmp) / 'layout.dylib'
    subprocess.run(['cc', '-dynamiclib', str(ROOT / 'src/tfu_layout.c'), '-o', str(libpath)], check=True)
    lib = C.CDLL(str(libpath))
    lib.tfu_layout_find.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32,
        C.POINTER(Layout), C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
    code, base, entry = code_section(IMAGE)
    layout, display, main = Layout(), C.c_uint32(), C.c_uint32()
    assert lib.tfu_layout_find(C.create_string_buffer(code), len(code), base, entry,
        C.byref(layout), C.byref(display), C.byref(main)) == 0
    hook, next_frame, completion = layout.movie_hook, layout.movie_next_frame, layout.movie_completion
    # Decode only complete instructions when locating the external calls and
    # completion branch. Addresses differ between retail and Steam executables.
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    next_instructions = list(decoder.disasm(read(next_frame, 0x71), next_frame))
    calls = [int(i.op_str, 16) for i in next_instructions if i.mnemonic == 'call']
    assert len(calls) in (3, 4), 'Unexpected NextFrame external-call layout'
    externals = set(calls) | {int(i.op_str,16) for i in next_instructions if i.mnemonic == 'jmp' and not next_frame <= int(i.op_str,16) < next_frame + 0x71}
    completion_instructions = list(decoder.disasm(read(completion, 27), completion))
    finish = next(int(i.op_str, 16) for i in completion_instructions if i.mnemonic == 'call')
    src = Path(tmp) / 'emit.c'
    exe = Path(tmp) / 'emit'
    src.write_text('''#include <stdio.h>
#include "tfu_movie_end.h"
#include <stdlib.h>
int main(int argc, char **argv) {
    unsigned char code[28], hook[8];
    tfu_movie_end_code_at(code, hook, strtoul(argv[1],0,0), strtoul(argv[2],0,0));
    fwrite(tfu_movie_end_completion, 1, sizeof(tfu_movie_end_completion), stdout);
    fwrite(code, 1, sizeof(code), stdout);
    fwrite(hook, 1, sizeof(hook), stdout);
}
''')
    subprocess.run(['cc', '-I', str(ROOT / 'src'), str(src), '-o', str(exe)], check=True)
    emitted = read(hook, 8) + subprocess.check_output([str(exe), str(hook), str(next_frame)])
assert len(emitted) == 61
assert read(hook, 8)[:4] == bytes.fromhex("891c24e8"), 'frame-advance signature mismatch'
assert read(completion, 17) == emitted[8:25], 'completion signature mismatch'


def run(frame, count, loop, patched):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    pages = {int(match,16) & ~4095 for i in next_instructions
             for match in re.findall(r'\[(0x[0-9a-f]+)\]', i.op_str)}
    pages |= {a & ~4095 for a in [next_frame, next_frame+0x70, hook, hook+8,
                    completion, completion+27, finish, *externals, 0x7f00f000, 0x10000000]}
    for address in pages:
        uc.mem_map(address, 4096)
    uc.mem_write(next_frame, read(next_frame, 0x71))  # original NextFrame, including rewind
    uc.mem_write(hook, read(hook, 8))
    uc.mem_write(completion, read(completion, 27))   # actual non-loop finish check
    if patched:
        uc.mem_write(0x7f00f000, emitted[25:53])
        uc.mem_write(hook, emitted[53:])
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
        if address in externals:
            if address == calls[1]:
                rewinds.append(struct.unpack('<I', uc.mem_read(uc.reg_read(UC_X86_REG_ESP) + 4, 4))[0])
            esp = uc.reg_read(UC_X86_REG_ESP)
            target = struct.unpack('<I', uc.mem_read(esp, 4))[0]
            uc.reg_write(UC_X86_REG_ESP, esp + 4)
            uc.reg_write(UC_X86_REG_EIP, target)
        if address in [finish, completion + 27]:
            uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, intercept)
    uc.emu_start(hook, hook + 8, count=100)
    assert uc.reg_read(UC_X86_REG_EIP) == hook + 8
    assert uc.reg_read(UC_X86_REG_ESP) == stack - 256
    assert uc.reg_read(UC_X86_REG_EBX) == decoder
    assert uc.reg_read(UC_X86_REG_EDI) == manager
    assert uc.reg_read(UC_X86_REG_ESI) == 0x12345678
    assert uc.reg_read(UC_X86_REG_EBP) == stack
    result_frame = struct.unpack('<I', uc.mem_read(decoder + 12, 4))[0]
    uc.emu_start(completion, 0, count=25)
    finished = uc.reg_read(UC_X86_REG_EIP) == finish
    return result_frame, bool(rewinds), finished


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
print(f'PASS {IMAGE.name}: reproduced EOF replay; {cases} boundary cases; intentional loops and register/stack preservation')
