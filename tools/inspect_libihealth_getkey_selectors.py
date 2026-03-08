#!/usr/bin/env python3
"""Resolve selector strings used by libiHealth's GenerateKap_getKey.

This targets a common PIC pattern in Thumb code:
  ldr rX, [pc, #imm]   @ 0xLITERAL_ADDR
  add rX, pc

The 32-bit word at LITERAL_ADDR is treated as a signed offset added to the
Thumb PC (addr_of_add + 4) to form a pointer.

We then attempt to read a null-terminated ASCII string at that pointer.

Usage examples:
  python3 tools/inspect_libihealth_getkey_selectors.py \
    vendor/ihealth/libiHealth.so --start 0x1564 --stop 0x15d0

Run multiple chunks to cover the whole function without huge output.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from elftools.elf.elffile import ELFFile


LDR_LIT_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+ldr\s+(r\d+),\s*\[pc,\s*#0x[0-9a-fA-F]+\]\s*@\s*(0x[0-9a-fA-F]+)\s*$"
)
ADD_PC_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+add\s+(r\d+),\s*pc\s*$"
)

# Thumb ADR (immediate) commonly used to reference nearby literal strings.
# Example line from objdump:
#   1fa0: a1b3          adr     r1, #716 <...>
ADR_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+adr\s+(r\d+),\s*#(-?\d+)\b.*$"
)


@dataclass(frozen=True)
class ResolvedPtr:
    reg: str
    ldr_addr: int
    lit_addr: int
    add_addr: int
    lit_value: int
    target_addr: int


def _read_u32_le_at_vaddr(elf: ELFFile, f, vaddr: int) -> int | None:
    for sec in elf.iter_sections():
        sh = sec.header
        start = int(sh["sh_addr"])
        end = start + int(sh["sh_size"])
        if start <= vaddr < end and (vaddr + 4) <= end:
            off = int(sh["sh_offset"]) + (vaddr - start)
            f.seek(off)
            b = f.read(4)
            return int.from_bytes(b, "little", signed=False)
    return None


def _read_c_string_at_vaddr(elf: ELFFile, f, vaddr: int, max_len: int = 128):
    for sec in elf.iter_sections():
        sh = sec.header
        start = int(sh["sh_addr"])
        end = start + int(sh["sh_size"])
        if start <= vaddr < end:
            off = int(sh["sh_offset"]) + (vaddr - start)
            f.seek(off)
            raw = f.read(max_len)
            s = raw.split(b"\x00", 1)[0]
            return sec.name, s
    return None, None


def _to_i32(u32: int) -> int:
    return u32 - 0x1_0000_0000 if u32 & 0x8000_0000 else u32


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("so", type=Path)
    ap.add_argument("--start", required=True)
    ap.add_argument("--stop", required=True)
    ap.add_argument("--arch", default="thumb")
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
    resolved: list[ResolvedPtr] = []
    adr_targets: list[tuple[int, str, int]] = []  # (instr_addr, reg, target_addr)

    # Parse sequentially; when we see ldr-literal into rX, record it. When we see
    # add rX, pc, resolve using the literal pool word.
    for line in dis:
        m = ADR_RE.match(line)
        if m:
            instr_addr = int(m.group(1), 16)
            reg = m.group(2)
            imm = int(m.group(3), 10)
            # Thumb PC in ALU ops is (addr of current instruction + 4).
            pc = instr_addr + 4
            target = (pc + imm) & 0xFFFF_FFFF
            adr_targets.append((instr_addr, reg, target))
            continue

        m = LDR_LIT_RE.match(line)
        if m:
            ldr_addr = int(m.group(1), 16)
            reg = m.group(2)
            lit_addr = int(m.group(3), 16)
            pending[reg] = (ldr_addr, lit_addr)
            continue
        m = ADD_PC_RE.match(line)
        if m:
            add_addr = int(m.group(1), 16)
            reg = m.group(2)
            if reg in pending:
                ldr_addr, lit_addr = pending.pop(reg)
                resolved.append(
                    ResolvedPtr(
                        reg=reg,
                        ldr_addr=ldr_addr,
                        lit_addr=lit_addr,
                        add_addr=add_addr,
                        lit_value=0,
                        target_addr=0,
                    )
                )

    with ns.so.open("rb") as f:
        elf = ELFFile(f)
        out: list[tuple[ResolvedPtr, str | None]] = []

        # Resolve ADR-referenced strings.
        for instr_addr, reg, target in adr_targets:
            sec, s = _read_c_string_at_vaddr(elf, f, target)
            if s is None:
                continue
            if not s:
                continue
            if any(b < 0x20 or b > 0x7E for b in s):
                continue
            out.append(
                (
                    ResolvedPtr(
                        reg=reg,
                        ldr_addr=0,
                        lit_addr=0,
                        add_addr=instr_addr,
                        lit_value=0,
                        target_addr=target,
                    ),
                    s.decode("utf-8", "replace"),
                )
            )

        for r in resolved:
            lit_u32 = _read_u32_le_at_vaddr(elf, f, r.lit_addr)
            if lit_u32 is None:
                continue
            lit_i32 = _to_i32(lit_u32)
            pc = r.add_addr + 4
            target = (pc + lit_i32) & 0xFFFF_FFFF
            sec, s = _read_c_string_at_vaddr(elf, f, target)
            if s is None:
                out.append((r.__class__(**{**r.__dict__, "lit_value": lit_u32, "target_addr": target}), None))
                continue
            # only keep mostly-printable strings
            if not s:
                continue
            if any(b < 0x20 or b > 0x7E for b in s):
                continue
            out.append((r.__class__(**{**r.__dict__, "lit_value": lit_u32, "target_addr": target}), s.decode("utf-8", "replace")))

    # de-dupe by (target,string)
    seen = set()
    printed = 0
    for r, s in out:
        key = (r.target_addr, s)
        if key in seen:
            continue
        seen.add(key)
        if s is None:
            continue
        print(
            f"reg={r.reg} ldr=0x{r.ldr_addr:x} lit=0x{r.lit_addr:x} "
            f"add=0x{r.add_addr:x} lit_u32=0x{r.lit_value:08x} -> ptr=0x{r.target_addr:x} str={s!r}"
        )
        printed += 1
        if printed >= ns.max:
            break

    if printed == 0:
        print("(no printable selector strings resolved in this range)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
