#!/usr/bin/env python3
"""Extract referenced rodata strings from libiHealth.so.

This is a lightweight helper for reverse-engineering/debugging the iHealth native
library at the "metadata" level (strings, addresses), without needing to run ARM
code locally.

It disassembles a small address range, collects literal references like:
  ldr r1, [pc, #...]  @ 0x1830
and then maps those virtual addresses back to ELF sections to read C-strings.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile


def read_c_string(elf: ELFFile, f, vaddr: int, max_len: int = 256):
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("so", type=Path)
    ap.add_argument("--arch", default="thumb")
    ap.add_argument("--start", required=True, help="Start address (hex like 0x1564)")
    ap.add_argument("--stop", required=True, help="Stop address (hex like 0x15d0)")
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
    )

    addrs = sorted({int(m.group(1), 16) for m in re.finditer(r"@\s*0x([0-9a-fA-F]+)", dis)})
    print("referenced_addrs", " ".join(hex(a) for a in addrs))

    with ns.so.open("rb") as f:
        elf = ELFFile(f)
        for a in addrs:
            sec, s = read_c_string(elf, f, a)
            if s is None:
                print(f"{hex(a)} <no-section>")
                continue
            try:
                ascii_s = s.decode("utf-8", "replace")
            except Exception:
                ascii_s = "<decode-error>"
            print(f"{hex(a)} {sec} len={len(s)} ascii={ascii_s!r} hex={s.hex()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
