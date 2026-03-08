#!/usr/bin/env python3
"""Extract 16-byte key blobs used by libiHealth's GenerateKap_getKey.

We discovered (via objdump) that Java_com_ihealth_communication_ins_GenerateKap_getKey
matches a selector string (e.g. "PO3") and then loads a pointer to a 16-byte blob
from a literal pool using the common Thumb PIC pattern:
  ldr r0, [pc, #imm]   @ 0xLITERAL_ADDR
  add r0, pc

Then it returns a new byte[] of length 0x10 populated from that blob.

This script parses the disassembly for those LDR+ADD sequences, resolves the
pointers in the ELF, and dumps the referenced bytes.

Usage:
  python3 tools/extract_libihealth_getkey_blobs.py vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so \
    --start 0x21f0 --stop 0x2270

Tip: Use nm -D -n to find the JNI symbol boundaries.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from elftools.elf.elffile import ELFFile


LDR_LIT_RE = re.compile(
    r"^\s*(?P<addr>[0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+ldr(?:\.w)?\s+(?P<reg>r\d+),\s*\[pc,\s*#(?P<imm>(?:0x)?[0-9a-fA-F]+)\]"
)
ADD_PC_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+add\s+(r\d+),\s*pc\s*$"
)


@dataclass(frozen=True)
class Resolved:
    reg: str
    ldr_addr: int
    lit_addr: int
    add_addr: int
    lit_u32: int
    target_addr: int
    blob_hex: str


def _to_i32(u32: int) -> int:
    return u32 - 0x1_0000_0000 if u32 & 0x8000_0000 else u32


def _thumb_pc_for_ldr_literal(insn_addr: int) -> int:
    # For Thumb LDR (literal): base is Align(PC, 4) where PC = insn_addr + 4
    return (insn_addr + 4) & ~0x3


def _read_bytes_at_vaddr(elf: ELFFile, f, vaddr: int, size: int) -> bytes | None:
    for sec in elf.iter_sections():
        sh = sec.header
        start = int(sh["sh_addr"])
        end = start + int(sh["sh_size"])
        if start <= vaddr < end and (vaddr + size) <= end:
            off = int(sh["sh_offset"]) + (vaddr - start)
            f.seek(off)
            return f.read(size)
    return None


def _read_u32_le_at_vaddr(elf: ELFFile, f, vaddr: int) -> int | None:
    b = _read_bytes_at_vaddr(elf, f, vaddr, 4)
    if b is None or len(b) != 4:
        return None
    return int.from_bytes(b, "little", signed=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("so", type=Path)
    ap.add_argument("--start", required=True)
    ap.add_argument("--stop", required=True)
    ap.add_argument("--arch", default="thumb")
    ap.add_argument("--len", type=int, default=16, help="Blob length to dump")
    ap.add_argument("--max", type=int, default=200)
    ns = ap.parse_args()

    start = int(ns.start, 16)
    stop = int(ns.stop, 16)

    dis = subprocess.check_output(
        [
            "objdump",
            "-d",
            f"--arch-name={ns.arch}",
            f"--start-address=0x{start:x}",
            f"--stop-address=0x{stop:x}",
            str(ns.so),
        ],
        text=True,
        errors="replace",
    ).splitlines()

    pending: dict[str, tuple[int, int]] = {}
    pairs: list[tuple[str, int, int, int]] = []  # (reg, ldr_addr, lit_addr, add_addr)

    for line in dis:
        m = LDR_LIT_RE.match(line)
        if m:
            ldr_addr = int(m.group("addr"), 16)
            reg = m.group("reg")
            imm_raw = m.group("imm").lower()
            if imm_raw.startswith("0x") or any(c in imm_raw for c in "abcdef"):
                imm = int(imm_raw, 16)
            else:
                imm = int(imm_raw, 10)
            lit_addr = _thumb_pc_for_ldr_literal(ldr_addr) + imm
            pending[reg] = (ldr_addr, lit_addr)
            continue
        m = ADD_PC_RE.match(line)
        if m:
            add_addr = int(m.group(1), 16)
            reg = m.group(2)
            if reg in pending:
                ldr_addr, lit_addr = pending.pop(reg)
                pairs.append((reg, ldr_addr, lit_addr, add_addr))

    resolved: list[Resolved] = []

    with ns.so.open("rb") as f:
        elf = ELFFile(f)
        for reg, ldr_addr, lit_addr, add_addr in pairs:
            lit_u32 = _read_u32_le_at_vaddr(elf, f, lit_addr)
            if lit_u32 is None:
                continue
            pc = add_addr + 4
            target = (pc + _to_i32(lit_u32)) & 0xFFFF_FFFF
            blob = _read_bytes_at_vaddr(elf, f, target, ns.len)
            if blob is None:
                continue
            resolved.append(
                Resolved(
                    reg=reg,
                    ldr_addr=ldr_addr,
                    lit_addr=lit_addr,
                    add_addr=add_addr,
                    lit_u32=lit_u32,
                    target_addr=target,
                    blob_hex=blob.hex(),
                )
            )

    # de-dupe by target
    seen = set()
    printed = 0
    for r in resolved:
        if r.target_addr in seen:
            continue
        seen.add(r.target_addr)
        print(
            f"reg={r.reg} ldr=0x{r.ldr_addr:x} lit=0x{r.lit_addr:x} add=0x{r.add_addr:x} "
            f"lit_u32=0x{r.lit_u32:08x} -> ptr=0x{r.target_addr:x} blob{ns.len}={r.blob_hex}"
        )
        printed += 1
        if printed >= ns.max:
            break

    if printed == 0:
        print("(no blobs resolved in this range)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
