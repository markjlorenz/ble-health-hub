#!/usr/bin/env python3
"""Dump Dalvik bytecode instructions for a specific class+method in an APK.

Usage:
  .venv/bin/python tools/dex_dump_method.py <apk> <dex_name> <class_name> <method_name>

Example:
  .venv/bin/python tools/dex_dump_method.py "iHealth MyVitals_4.13.1_APKPure.apk" classes4.dex \
    Lcom/ihealth/communication/ins/AcInsSet; haveNewData
"""

from __future__ import annotations

import argparse
import logging

from androguard.core.apk import APK
from androguard.core.dex import DEX


def _silence_loguru() -> None:
    try:
        from loguru import logger  # type: ignore

        logger.remove()
    except Exception:
        pass


def main() -> None:
    logging.getLogger().setLevel(logging.WARNING)
    logging.getLogger("androguard").setLevel(logging.WARNING)
    _silence_loguru()

    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("dex", help="DEX filename inside APK, e.g. classes4.dex")
    ap.add_argument("cls", help="Dalvik class name, e.g. Lcom/pkg/Foo;")
    ap.add_argument("method", help="Method name, e.g. haveNewData")
    ap.add_argument("--max-bytes", type=int, default=5000)
    ns = ap.parse_args()

    apk = APK(ns.apk)
    dex_bytes = apk.get_file(ns.dex)
    if dex_bytes is None:
        raise SystemExit(f"DEX not found in apk: {ns.dex}")

    dex = DEX(dex_bytes)
    classes = {c.get_name(): c for c in dex.get_classes()}
    cls = classes.get(ns.cls)
    if cls is None:
        raise SystemExit(f"Class not found in {ns.dex}: {ns.cls}")

    methods = [m for m in cls.get_methods() if m.get_name() == ns.method]
    if not methods:
        raise SystemExit(f"Method not found: {ns.cls}->{ns.method}")

    for m in methods:
        code = m.get_code()
        if code is None:
            print(f"== {ns.cls}->{m.get_name()}{m.get_descriptor()} (no code)")
            continue

        bc = code.get_bc()
        print(f"== {ns.cls}->{m.get_name()}{m.get_descriptor()} regs={code.registers_size} insns_units={code.get_length()}")

        addr = 0
        for ins in bc.get_instructions():
            if addr >= ns.max_bytes:
                print(f"... truncated at {addr} bytes")
                break
            print(f"{addr:04x}: {ins.get_name():24s} {ins.get_output()}")
            addr += ins.get_length()


if __name__ == "__main__":
    main()
