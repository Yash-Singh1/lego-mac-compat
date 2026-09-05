#!/usr/bin/env python3
"""Recover the plain Mach-O from LEGO Pirates' packed LEGOPirates.macbin.

The shipped executable exposes one RWX segment and a decompression stub (a
custom NRV/UPX-derived format that stock UPX rejects).  Instead of reverse
engineering the compressor, run the stub's two stages in Unicorn, answering
its mmap/mprotect syscalls, and stop right before it would hand the
reconstructed image to dyld.  The Mach-O header the stub built in memory then
describes where every segment lives, and the file is rebuilt from those
segments.  The result still contains the SecuROM activation code; nothing is
bypassed here.

Usage: unpack_securom.py <packed LEGOPirates.macbin> <output image>
Requires the `unicorn` Python package (pip3 install unicorn).
"""

import struct
import sys
from pathlib import Path

try:
    from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
    from unicorn.x86_const import (
        UC_X86_REG_EAX,
        UC_X86_REG_ECX,
        UC_X86_REG_EDX,
        UC_X86_REG_EFLAGS,
        UC_X86_REG_EIP,
        UC_X86_REG_ESP,
    )
except ImportError:
    sys.exit("unpack_securom.py: the 'unicorn' package is required "
             "(pip3 install unicorn)")

# Packer stub layout of the 2011 Mac build.  The distribution channels ship
# differently packed files, so nothing here is checked against a digest; the
# stub either reaches the dyld hand-off or the run fails with the address it
# stopped at.
IMAGE_BASE = 0x00E56000
IMAGE_MAP_SIZE = 0x0049A000
ENTRY = 0x012EE474
STAGE2 = 0x012EEA28
SYSENTER = 0x012EEA6E
BEFORE_DYLD_OPEN = 0x012EEDD9
STACK_BASE = 0x70000000
STACK_SIZE = 0x00200000


def align_down(value: int, alignment: int = 0x1000) -> int:
    return value & -alignment


def align_up(value: int, alignment: int = 0x1000) -> int:
    return (value + alignment - 1) & -alignment


def ensure_mapped(emu: Uc, address: int, length: int) -> None:
    start = align_down(address)
    end = align_up(address + length)
    mapped = [(lo, hi + 1) for lo, hi, _perms in emu.mem_regions()]
    cursor = start
    while cursor < end:
        containing = next(((lo, hi) for lo, hi in mapped if lo <= cursor < hi), None)
        if containing:
            cursor = min(end, containing[1])
            continue
        next_start = min((lo for lo, _hi in mapped if lo > cursor), default=end)
        run_end = min(end, next_start)
        emu.mem_map(cursor, run_end - cursor)
        mapped.append((cursor, run_end))
        cursor = run_end


def u32(emu: Uc, address: int) -> int:
    return struct.unpack("<I", bytes(emu.mem_read(address, 4)))[0]


def rebuild_macho(emu: Uc) -> bytes:
    magic = b"\xce\xfa\xed\xfe"
    candidates: list[int] = []
    for lo, hi, _perms in emu.mem_regions():
        if lo >= IMAGE_BASE or hi - lo > 0x10000000:
            continue
        data = bytes(emu.mem_read(lo, hi - lo + 1))
        offset = data.find(magic)
        while offset >= 0:
            candidates.append(lo + offset)
            offset = data.find(magic, offset + 1)

    for header_address in candidates:
        try:
            header = bytes(emu.mem_read(header_address, 28))
            (_magic, _cpu, _subcpu, filetype, ncmds, sizeofcmds, _flags) = struct.unpack(
                "<IiiIIII", header
            )
            if filetype != 2 or ncmds > 1000 or sizeofcmds > 0x100000:
                continue
            commands = bytes(emu.mem_read(header_address + 28, sizeofcmds))
            segments: list[tuple[str, int, int, int, int]] = []
            offset = 0
            for _ in range(ncmds):
                cmd, cmdsize = struct.unpack_from("<II", commands, offset)
                if cmdsize < 8 or offset + cmdsize > len(commands):
                    raise ValueError("invalid load command")
                if cmd == 1 and cmdsize >= 56:
                    fields = struct.unpack_from("<II16sIIIIIIII", commands, offset)
                    name = fields[2].split(b"\0", 1)[0].decode("ascii", "replace")
                    vmaddr, vmsize, fileoff, filesize = fields[3:7]
                    segments.append((name, vmaddr, vmsize, fileoff, filesize))
                offset += cmdsize
            if not segments:
                continue
            output_size = max(fileoff + filesize for _, _, _, fileoff, filesize in segments)
            output = bytearray(output_size)
            for name, vmaddr, _vmsize, fileoff, filesize in segments:
                print(f"  segment {name:<12} vm=0x{vmaddr:08x} file=0x{fileoff:x}+0x{filesize:x}")
                if filesize:
                    output[fileoff : fileoff + filesize] = emu.mem_read(vmaddr, filesize)
            return bytes(output)
        except Exception as error:  # noqa: BLE001 - try the next candidate
            print(f"  candidate 0x{header_address:08x} rejected: {error}")
    raise RuntimeError("no reconstructed Mach-O header found")


def unpack(packed: bytes) -> bytes:
    emu = Uc(UC_ARCH_X86, UC_MODE_32)
    emu.mem_map(IMAGE_BASE, max(IMAGE_MAP_SIZE, align_up(len(packed))))
    emu.mem_write(IMAGE_BASE, packed)
    emu.mem_map(STACK_BASE, STACK_SIZE)
    emu.reg_write(UC_X86_REG_ESP, STACK_BASE + STACK_SIZE - 0x1000)

    state = {"stage2": False, "dyld": False, "cursor": 0x50000000}

    def on_code(uc: Uc, address: int, _size: int, _user_data: object) -> None:
        if address == STAGE2:
            state["stage2"] = True
            return
        if address == BEFORE_DYLD_OPEN:
            state["dyld"] = True
            uc.emu_stop()
            return
        if address != SYSENTER:
            return

        # EAX is the Darwin i386 syscall number; ECX points at the caller's stack.
        syscall = uc.reg_read(UC_X86_REG_EAX)
        args_pointer = uc.reg_read(UC_X86_REG_ECX) + 4
        args = [u32(uc, args_pointer + 4 * i) for i in range(8)]
        result = 0
        if syscall == 197:  # mmap(addr, len, prot, flags, fd, offset)
            requested, length, flags = args[0], args[1], args[3]
            if requested == 0 and not (flags & 0x10):  # not MAP_FIXED
                requested = align_up(state["cursor"])
                state["cursor"] = requested + align_up(length)
            ensure_mapped(uc, requested, length)
            result = requested
        elif syscall in (73, 74):  # munmap (kept mapped) / mprotect
            pass
        elif syscall == 1:
            raise RuntimeError(f"packer stub called exit({args[0]})")
        else:
            raise RuntimeError(f"unhandled syscall {syscall} args={args}")

        uc.reg_write(UC_X86_REG_EAX, result & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) & ~1)
        uc.reg_write(UC_X86_REG_EIP, uc.reg_read(UC_X86_REG_EDX))

    for marker in (STAGE2, BEFORE_DYLD_OPEN, SYSENTER):
        emu.hook_add(UC_HOOK_CODE, on_code, begin=marker, end=marker)
    emu.emu_start(ENTRY, 0, count=500_000_000)

    if not state["dyld"]:
        raise RuntimeError(
            f"stub did not reach the dyld hand-off (stage2={state['stage2']}, "
            f"eip=0x{emu.reg_read(UC_X86_REG_EIP):08x})"
        )
    return rebuild_macho(emu)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    source, destination = Path(argv[1]), Path(argv[2])
    packed = source.read_bytes()
    print(f"unpacking {source} ({len(packed)} bytes)")
    image = unpack(packed)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(image)
    destination.chmod(0o755)
    print(f"wrote {destination} ({len(image)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
