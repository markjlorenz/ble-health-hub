#!/usr/bin/env python3
"""Scan APK DEX bytecode for callsites into GenerateKap.

Goal: find where the app constructs/validates vendor handshake frames.

This is intentionally static (no decompilation). It walks Dalvik bytecode and
records methods that contain invoke-* instructions referencing GenerateKap.
"""

from __future__ import annotations

import sys


def _silence_loguru() -> None:
    try:
        from loguru import logger

        logger.remove()
        logger.add(sys.stderr, level="ERROR")
    except Exception:
        return


def main() -> int:
    _silence_loguru()

    from androguard.core.apk import APK
    from androguard.core.dex import DEX

    apk_path = sys.argv[1] if len(sys.argv) > 1 else "iHealth MyVitals_4.13.1_APKPure.apk"

    apk = APK(apk_path)
    dex_names = list(apk.get_dex_names())

    needles = [
        "Lcom/ihealth/communication/ins/GenerateKap;->getKey",
        "Lcom/ihealth/communication/ins/GenerateKap;->getKa",
        "Lcom/ihealth/communication/ins/GenerateKap;->decrypt",
        "Lcom/ihealth/communication/ins/GenerateKap;->processSampleDataByte",
        "Lcom/ihealth/communication/ins/GenerateKap;->processSampleDataShort",
    ]

    results: list[tuple[str, str, str, str, str]] = []

    for dex_name in dex_names:
        dex_bytes = apk.get_file(dex_name)
        if not dex_bytes:
            continue
        dex = DEX(dex_bytes)

        for cls in dex.get_classes():
            cls_name = cls.get_name()
            for m in cls.get_methods():
                code = m.get_code()
                if not code:
                    continue
                for ins in code.get_bc().get_instructions():
                    if not ins.get_name().startswith("invoke"):
                        continue
                    out = ins.get_output()
                    for nd in needles:
                        if nd in out:
                            results.append(
                                (dex_name, nd, cls_name, m.get_name(), m.get_descriptor())
                            )

    print("dex files:", dex_names)
    print("total invoke hits:", len(results))

    by_needle: dict[str, set[tuple[str, str, str, str]]] = {}
    for dex_name, nd, cls_name, mn, md in results:
        by_needle.setdefault(nd, set()).add((dex_name, cls_name, mn, md))

    for nd in needles:
        methods = sorted(by_needle.get(nd, set()))
        print(f"\n== {nd}  unique methods: {len(methods)}")
        for dex_name, cls_name, mn, md in methods[:200]:
            print(" ", dex_name, cls_name, mn, md)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
