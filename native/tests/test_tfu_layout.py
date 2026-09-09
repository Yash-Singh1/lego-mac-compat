#!/usr/bin/env python3
"""Discover patch sites in real Mac images, then relocate code and reject damage.

Usage: python3 tests/test_tfu_layout.py [TFU_IMAGE ...]
No game is launched. The C resolver sees buffers at arbitrary virtual bases.
"""
import ctypes as C
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIELDS = 'xinput_state xinput_caps xinput_vibration input_update input_tail input_evaluator movie_hook movie_next_frame movie_completion'.split()
class Layout(C.Structure):
    _fields_ = [(field, C.c_uint32) for field in FIELDS]

def code_section(path):
    data = path.read_bytes()
    assert data[:4] == bytes.fromhex('cefaedfe'), 'Supply a thin i386 image'
    offset, entry, section = 28, None, None
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 1:
            for i in range(struct.unpack_from('<I', data, offset + 48)[0]):
                name, segment, address, length, pos = struct.unpack_from('<16s16sIII', data, offset + 56 + i * 68)
                if name.rstrip(b'\0') == b'__text' and segment.rstrip(b'\0') == b'__TEXT':
                    section = data[pos:pos + length], address
        if command == 5:  # LC_UNIXTHREAD x86_THREAD_STATE32 eip
            entry = struct.unpack_from('<I', data, offset + 16 + 10 * 4)[0]
        offset += size
    assert section and entry
    return *section, entry

if __name__ == '__main__':
    paths = list(map(Path, sys.argv[1:])) or [ROOT / 'build/TFU-Compat.app/Contents/SharedSupport/TFU.image',
        Path.home() / 'Library/Application Support/Steam/steamapps/common/Star Wars The Force Unleashed/Star Wars The Force Unleashed.app/Contents/MacOS/Star Wars The Force Unleashed']
    with tempfile.TemporaryDirectory(prefix='tfu-layout-') as tmp:
        libpath = Path(tmp) / 'layout.dylib'
        subprocess.run(['cc', '-dynamiclib', '-O2', '-Wall', '-Wextra', '-Werror',
            str(ROOT / 'src/tfu_layout.c'), '-o', str(libpath)], check=True)
        lib = C.CDLL(str(libpath))
        lib.tfu_layout_find.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32,
            C.POINTER(Layout), C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
        def resolve(code, base, entry):
            output, display, main = Layout(), C.c_uint32(), C.c_uint32()
            buffer = C.create_string_buffer(bytes(code))
            status = lib.tfu_layout_find(buffer, len(code), base, entry, C.byref(output), C.byref(display), C.byref(main))
            return status, [getattr(output, f) for f in FIELDS] + [display.value, main.value]
        for path in paths:
            code, base, entry = code_section(path)
            status, sites = resolve(code, base, entry)
            assert status == 0, path
            for delta in [0x05000000, 0x12345000]:
                relocated, shifted = resolve(code, base + delta, entry + delta)
                assert relocated == 0 and shifted == [a + delta for a in sites], path
            relinked = bytearray(code)
            start = sites[2] - base
            operand = code.index(bytes.fromhex('8b1cd5'), start, start + 64) + 3
            struct.pack_into('<I', relinked, operand, 0x13579bdf)
            assert resolve(relinked, base, entry) == (0, sites), 'Linked controller address was treated as code'
            damaged = bytearray(code)
            damaged[sites[0] - base] = 0x90
            assert resolve(damaged, base, entry)[0] != 0, 'Missing controller routine accepted'
            assert resolve(code + code, base, entry)[0] != 0, 'Ambiguous routine accepted'
            damaged = bytearray(code)
            struct.pack_into('<i', damaged, sites[4] - base + 11, 0)
            assert resolve(damaged, base, entry)[0] != 0, 'Invalid evaluator target accepted'
            assert resolve(code, base, entry + 1)[0] != 0, 'Invalid entry accepted'
            print(f'PASS {path.name}: discovery, relocation/relinking, missing/ambiguous/invalid routines rejected')
