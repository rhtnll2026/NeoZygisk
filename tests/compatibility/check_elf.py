#!/usr/bin/env python3
"""Check the actual module ZIPs, including PIE executables, without running them."""

import argparse
import struct
import sys
import zipfile
from pathlib import Path

ABIS = {"arm64-v8a": (2, 183), "armeabi-v7a": (1, 40),
        "x86": (1, 3), "x86_64": (2, 62)}
PAGE = 16384
PT_LOAD = 1
PT_GNU_RELRO = 0x6474E552


def check_elf(data, abi):
    """Raise ValueError for a missing, malformed, wrong-ABI or unaligned ELF."""
    elf_class, machine = ABIS[abi]
    if len(data) < 16 or data[:4] != b"\x7fELF" or data[4:7] != bytes((elf_class, 1, 1)):
        raise ValueError("wrong ELF class, encoding or magic")
    header_format = "<HHIQQQIHHHHHH" if elf_class == 2 else "<HHIIIIIHHHHHH"
    ph_format = "<IIQQQQQQ" if elf_class == 2 else "<IIIIIIII"
    header_size = 16 + struct.calcsize(header_format)
    if len(data) < header_size:
        raise ValueError("truncated ELF header")
    header = struct.unpack_from(header_format, data, 16)
    if header[0] != 3 or header[1] != machine or header[2] != 1:
        raise ValueError("expected an Android shared object or PIE for " + abi)
    phoff, phsize, phnum = header[4], header[8], header[9]
    if (header[7] != header_size or phoff < header_size or not phnum
            or phsize != struct.calcsize(ph_format) or phoff + phsize * phnum > len(data)):
        raise ValueError("invalid program header table")
    segments = []
    for i in range(phnum):
        p = struct.unpack_from(ph_format, data, phoff + i * phsize)
        if elf_class == 2:
            kind, flags, offset, address, _, file_size, mem_size, align = p
        else:
            kind, offset, address, _, file_size, mem_size, flags, align = p
        if kind not in (PT_LOAD, PT_GNU_RELRO):
            continue
        if file_size > mem_size or offset + file_size > len(data):
            raise ValueError("invalid segment bounds")
        segments.append((kind, flags, offset, address, mem_size, align))
    loads = [s for s in segments if s[0] == PT_LOAD]
    if not loads:
        raise ValueError("missing PT_LOAD")
    if elf_class != 2:
        return  # Android's 16 KiB mode applies to the 64-bit ABIs.
    for _, _, offset, address, _, align in loads:
        if align < PAGE or align & (align - 1) or (address - offset) % PAGE:
            raise ValueError("PT_LOAD is not 16 KiB aligned")
    for kind, _, _, address, size, _ in segments:
        if kind != PT_GNU_RELRO or size == 0:
            continue
        end = address + size
        if end % PAGE == 0:
            continue
        for _, flags, _, load_address, load_size, _ in loads:
            if flags & 2 and load_address <= address < load_address + load_size:
                if end < load_address + load_size:
                    raise ValueError("RELRO shares its trailing 16 KiB page with writable data")


def check_zip(path):
    failures = []
    checked = 0
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        for abi in ABIS:
            for name in (f"bin/{abi}/zygiskd", f"lib/{abi}/libzygisk.so",
                         f"lib/{abi}/libzygisk_ptrace.so"):
                try:
                    if names.count(name) != 1:
                        raise ValueError("expected exactly one packaged binary")
                    check_elf(archive.read(name), abi)
                    checked += 1
                except (ValueError, zipfile.BadZipFile) as error:
                    failures.append(f"{name}: {error}")
    if failures:
        raise ValueError("\n".join(failures))
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    failed = False
    for path in args.archives:
        try:
            count = check_zip(path)
            print(f"PASS {path.name}: {count} native binaries; all 64-bit LOAD/RELRO layouts support 16 KiB")
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            print(f"FAIL {path}: {error}", file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
