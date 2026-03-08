#!/usr/bin/env python3
"""Extract the PO3 16-byte key seed blob from libiHealth.so.

This uses the already-reversed control flow:
- In Java_com_ihealth_communication_ins_GenerateKap_getKey(), the PO3 selector
  branches to 0x2202.
- The PO3 case loads a 32-bit literal from 0x2380, then does `add r0, pc`.

We map VMAs to file offsets via ELF section headers and then dump the referenced
bytes.

Run:
  python tools/elf_extract_po3_blob.py vendor/.../libiHealth.armeabi-v7a.so
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Section:
    name: str
    vma: int
    offset: int
    size: int

    def contains_vma(self, addr: int) -> bool:
        return self.size > 0 and self.vma <= addr < self.vma + self.size

    def vma_to_file(self, addr: int) -> int:
        if not self.contains_vma(addr):
            raise ValueError(f"0x{addr:x} not in section {self.name}")
        return self.offset + (addr - self.vma)


def parse_elf32_sections(elf_bytes: bytes) -> list[Section]:
    if elf_bytes[:4] != b"\x7fELF":
        raise ValueError("Not an ELF file")
    ei_class = elf_bytes[4]
    ei_data = elf_bytes[5]
    if ei_class != 1 or ei_data != 1:
        raise ValueError(f"Unsupported ELF class/data {ei_class}/{ei_data} (need ELF32 LE)")

    e_shoff = struct.unpack_from("<I", elf_bytes, 0x20)[0]
    e_shentsize = struct.unpack_from("<H", elf_bytes, 0x2E)[0]
    e_shnum = struct.unpack_from("<H", elf_bytes, 0x30)[0]
    e_shstrndx = struct.unpack_from("<H", elf_bytes, 0x32)[0]

    def shdr(i: int) -> tuple[int, int, int, int, int, int, int, int, int, int]:
        off = e_shoff + i * e_shentsize
        return struct.unpack_from("<IIIIIIIIII", elf_bytes, off)

    sh_name, _sh_type, _sh_flags, _sh_addr, sh_offset, sh_size, *_ = shdr(e_shstrndx)
    shstrtab = elf_bytes[sh_offset : sh_offset + sh_size]

    def sec_name(name_off: int) -> str:
        end = shstrtab.find(b"\x00", name_off)
        if end < 0:
            end = len(shstrtab)
        return shstrtab[name_off:end].decode("ascii", "replace")

    sections: list[Section] = []
    for i in range(e_shnum):
        sh_name, _sh_type, _sh_flags, sh_addr, sh_offset, sh_size, *_ = shdr(i)
        sections.append(Section(sec_name(sh_name), sh_addr, sh_offset, sh_size))
    return sections


def u32_le(b: bytes, file_off: int) -> int:
    return struct.unpack_from("<I", b, file_off)[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", type=Path)
    args = ap.parse_args()

    elf_path: Path = args.elf
    elf_bytes = elf_path.read_bytes()
    sections = parse_elf32_sections(elf_bytes)

    def find_section(name: str) -> Section:
        for s in sections:
            if s.name == name:
                return s
        raise SystemExit(f"Missing section: {name}")

    text = find_section(".text")

    # Addresses from native disassembly:
    #   PO3 branch target: 0x2202
    #   literal used by PO3 case: 0x2380
    #   add r0, pc at 0x220a; Thumb PC at that point is (instr_addr + 4).
    po3_lit_vma = 0x2380
    add_instr_vma = 0x220A
    add_pc = add_instr_vma + 4

    lit_file_off = text.vma_to_file(po3_lit_vma)
    lit_u32 = u32_le(elf_bytes, lit_file_off)
    lit_s32 = lit_u32 - (1 << 32) if (lit_u32 & 0x8000_0000) else lit_u32

    ptr = (add_pc + lit_s32) & 0xFFFF_FFFF

    print(f".text: vma=0x{text.vma:x} off=0x{text.offset:x} size=0x{text.size:x}")
    print(f"PO3 literal @0x{po3_lit_vma:x} (file+0x{lit_file_off:x}): 0x{lit_u32:08x} (s32 {lit_s32})")
    print(f"PO3 ptr = add_pc(0x{add_pc:x}) + lit_s32({lit_s32}) = 0x{ptr:08x}")

    containing = next((s for s in sections if s.contains_vma(ptr)), None)
    if containing is None:
        raise SystemExit(f"Pointer 0x{ptr:x} not within any section; cannot dump bytes")

    ptr_file_off = containing.vma_to_file(ptr)
    dump = elf_bytes[ptr_file_off : ptr_file_off + 64]

    print(f"ptr section: {containing.name} (vma=0x{containing.vma:x} off=0x{containing.offset:x} size=0x{containing.size:x})")
    print(f"bytes @0x{ptr:08x} (file+0x{ptr_file_off:x}): {dump.hex()}")
    print(f"PO3 blob16: {dump[:16].hex()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
