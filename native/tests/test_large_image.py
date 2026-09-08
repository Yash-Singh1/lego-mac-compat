#!/usr/bin/env python3
"""Original synthetic Mach-O: large BSS, >512 imports, and defined indirect symbols."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]
pack = struct.pack

def section(name, segment, address, size, offset, flags=0, index=0, stride=0):
    return pack('<16s16s9I', name.encode(), segment.encode(), address, size, offset, 2, 0, 0, flags, index, stride)

def segment(name, address, size, offset, file_size, protection, sections=()):
    return pack('<II16s8I', 1, 56 + 68 * len(sections), name.encode(), address, size, offset, file_size, 7, protection, len(sections), 0) + b''.join(sections)

with tempfile.TemporaryDirectory(prefix='lp32-main-image-') as tmp:
    root = Path(tmp)
    init_address, local_address = 0x1800, 0x1f00
    data_address, import_address, link_address = 0x02000000, 0x02801000, 0x02804000
    stub_address = import_address + 0x1000
    imports = 703  # 700 abs references, malloc, free, and a local function.
    own_stub = stub_address + (imports - 1) * 5
    malloc_stub, free_stub = own_stub - 10, own_stub - 5
    last_abs_stub = malloc_stub - 5
    assembly = f'''.text
.globl _probe
_probe:
 pushl %ebp
 movl %esp, %ebp
 subl $24, %esp
 movl {import_address + 4}, %eax
 testl $0x100, 440(%eax)
 jz fail
 cmpl $97, 1336(%eax)
 jne fail
 movl $-7, (%esp)
 movl ${last_abs_stub}, %eax
 calll *%eax
 cmpl $7, %eax
 jne fail
 movl ${own_stub}, %eax
 calll *%eax
 cmpl $42, %eax
 jne fail
 movl {import_address}, %eax
 calll *%eax
 cmpl $42, %eax
 jne fail
 movl $4096, (%esp)
 movl ${malloc_stub}, %eax
 calll *%eax
 testl %eax, %eax
 jz fail
 cmpl $0x12345678, {data_address + 8}
 jne fail
 movl %eax, (%esp)
 movl ${free_stub}, %eax
 calll *%eax
 leave
 ret
fail:
 ud2
'''
    source, obj = root / 'probe.S', root / 'probe.o'
    source.write_text(assembly)
    subprocess.run(['xcrun', 'clang', '-target', 'i386-apple-macos10.5', '-c', str(source), '-o', str(obj)], check=True)
    object_bytes = obj.read_bytes()
    cursor = 28
    code = None
    for _ in range(struct.unpack_from('<I', object_bytes, 16)[0]):
        command, size = struct.unpack_from('<II', object_bytes, cursor)
        if command == 1:
            for i in range(struct.unpack_from('<I', object_bytes, cursor + 48)[0]):
                fields = struct.unpack_from('<16s16s9I', object_bytes, cursor + 56 + 68 * i)
                if fields[0].rstrip(b'\0') == b'__text':
                    assert fields[7] == 0, 'probe must not require relocations'
                    code = object_bytes[fields[4]:fields[4] + fields[3]]
        cursor += size
    assert code is not None and len(code) < 0x700
    names = ['_abs', '_malloc', '_free', '_local', '__DefaultRuneLocale']
    strings = bytearray(b'\0')
    symbols = bytearray()
    for i, name in enumerate(names):
        symbols += pack('<IBBHI', len(strings), 0x0f if i == 3 else 1, 1 if i == 3 else 0, 0, local_address if i == 3 else 0)
        strings += name.encode() + b'\0'
    indirect = pack('<' + 'I' * (imports + 2), 3, 4, *([0] * 700 + [1, 2, 3]))
    link_offset = 0x5000
    sym_offset, str_offset = link_offset, link_offset + len(symbols)
    ind_offset = (str_offset + len(strings) + 3) & ~3
    reloc_offset = ind_offset + len(indirect)
    relocations = pack('<6I', data_address + 24 - 0x1000, 0x0c000003,
                       data_address + 28 - 0x1000, 0x0c000000,
                       data_address + 32 - 0x1000, 0x0d000003)
    commands = [
        segment('__TEXT', 0x1000, 0x1000, 0, 0x1000, 5,
                [section('__text', '__TEXT', init_address, 0x706, 0x800, 0x80000400)]),
        segment('__DATA', data_address, import_address - data_address, 0x1000, 0x1000, 3,
                [section('__mod_init_func', '__DATA', data_address + 16, 4, 0x1010, 9)]),
        segment('__IMPORT', import_address, 0x3000, 0x2000, 0x3000, 7,
                [section('__pointers', '__IMPORT', import_address, 8, 0x2000, 6),
                 section('__jump_table', '__IMPORT', stub_address, imports * 5, 0x3000, 8, 2, 5)]),
        segment('__LINKEDIT', link_address, 0x1000, link_offset, 0x1000, 1),
        pack('<6I', 2, 24, sym_offset, 5, str_offset, len(strings)),
        pack('<20I', 11, 80, 0, 0, 3, 1, 0, 3, 0, 0, 0, 0, 0, 0, ind_offset, imports + 2, reloc_offset, 3, 0, 0),
        pack('<4I', 5, 80, 1, 16) + pack('<16I', *([0] * 10 + [init_address] + [0] * 5)),
    ]
    image = bytearray(0x6000)
    header = pack('<7I', 0xfeedface, 7, 3, 2, len(commands), sum(map(len, commands)), 0)
    image[:len(header)] = header
    image[28:28 + sum(map(len, commands))] = b''.join(commands)
    image[0x800:0x800 + len(code)] = code
    image[0xf00:0xf06] = b'\xb8\x2a\0\0\0\xc3'
    struct.pack_into('<I', image, 0x1008, 0x12345678)
    struct.pack_into('<I', image, 0x1010, init_address)
    struct.pack_into('<3I', image, 0x1018, 4, 8, (-data_address - 32) & 0xffffffff)
    image[0x3000:0x3000 + imports * 5] = b'\xf4' * (imports * 5)
    image[sym_offset:sym_offset + len(symbols)] = symbols
    image[str_offset:str_offset + len(strings)] = strings
    image[ind_offset:ind_offset + len(indirect)] = indirect
    image[reloc_offset:reloc_offset + len(relocations)] = relocations
    executable = root / 'fixture'
    executable.write_bytes(image)
    checker = root / 'relocation_check.c'
    checker.write_text(r'''#include "macho_loader.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t resolve(const char *name, void *context) {
    (void)context; return strcmp(name, "_abs") ? 0 : 0x12340000;
}
int main(int argc, char **argv) {
    struct macho_image32 image;
    if (macho_image32_load(argv[1], &image)) return 1;
    int status = macho_image32_bind_external_relocations(&image, resolve, NULL);
    if (argc == 3) return status == -1 ? 0 : 2;
    if (status || *(uint32_t *)(uintptr_t)0x02000018 != 0x1f04 ||
        *(uint32_t *)(uintptr_t)0x0200001c != 0x12340008 ||
        *(uint32_t *)(uintptr_t)0x02000020 != (uint32_t)(0x1f00 - 0x02000020)) return 3;
    puts("Main external relocations: PASS (defined, external, PC-relative)");
    return 0;
}
''')
    checker_bin = root / 'relocation_check'
    subprocess.run(['xcrun', 'clang', '-arch', 'x86_64', '-Wall', '-Wextra', '-Werror',
                    '-Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000,-segaddr,__LEGACY,0x1000,-segprot,__LEGACY,rwx,rwx',
                    '-I' + str(NATIVE / 'src'), str(checker), str(NATIVE / 'src/macho_loader.c'),
                    str(NATIVE / 'src/macho_file.c'), str(NATIVE / 'src/address_space_reserve.S'),
                    '-o', str(checker_bin)], check=True)
    subprocess.run(['arch', '-x86_64', str(checker_bin), str(executable)], check=True, timeout=30)
    malformed = root / 'invalid-relocation'
    struct.pack_into('<I', image, reloc_offset, 0x20000 - 0x1000)
    malformed.write_bytes(image)
    subprocess.run(['arch', '-x86_64', str(checker_bin), str(malformed), 'invalid'], check=True, timeout=30)
    env = dict(os.environ, LP32_GAME='portal2', LP32_INITIALIZERS_SELFTEST='1')
    subprocess.run(['arch', '-x86_64', str(NATIVE / 'build/game_loader'), str(executable)], env=env, check=True, timeout=30)
    print('Large main-image fixture: PASS (702 imports, local stub/pointer, BSS canary)')
