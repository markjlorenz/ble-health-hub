#!/usr/bin/env python3
"""Generic DEX callsite scanner for the MyVitals APK.

Usage:
  .venv/bin/python tools/scan_callsites.py <apk> <substring>

Example:
  .venv/bin/python tools/scan_callsites.py "iHealth MyVitals_4.13.1_APKPure.apk" \
    "Lcom/ihealth/communication/ins/IdentifyIns;->identify"
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

    if len(sys.argv) < 3:
        print("usage: scan_callsites.py <apk_path> <needle_substring>", file=sys.stderr)
        return 2

    apk_path = sys.argv[1]
    needle = sys.argv[2]

    from androguard.core.apk import APK
    from androguard.core.dex import DEX

    apk = APK(apk_path)
    dex_names = list(apk.get_dex_names())

    hits: list[tuple[str, str, str, str]] = []

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
                    if needle in out:
                        hits.append((dex_name, cls_name, m.get_name(), m.get_descriptor()))

    print("dex files:", dex_names)
    print("needle:", needle)
    print("hit count:", len(hits))

    for dex_name, cls_name, mn, md in sorted(set(hits))[:500]:
        print(dex_name, cls_name, mn, md)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
