#!/usr/bin/env python3
"""Find which DEX file inside an APK contains a given class.

Usage:
  .venv/bin/python tools/find_class_dex.py <apk> <class_name>

Example:
  .venv/bin/python tools/find_class_dex.py "iHealth MyVitals_4.13.1_APKPure.apk" \
    Lcom/ihealth/communication/base/protocol/BleCommProtocol;
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
    ap.add_argument("class_name")
    ns = ap.parse_args()

    apk = APK(ns.apk)
    dex_names = [n for n in apk.get_files() if n.endswith('.dex')]
    dex_names.sort(key=lambda s: (len(s), s))

    found = []
    for dex_name in dex_names:
        data = apk.get_file(dex_name)
        if data is None:
            continue
        d = DEX(data)
        classes = {c.get_name() for c in d.get_classes()}
        if ns.class_name in classes:
            found.append(dex_name)

    if not found:
        raise SystemExit(f"Not found: {ns.class_name}")

    for dex_name in found:
        print(dex_name)


if __name__ == '__main__':
    main()
