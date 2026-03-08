#!/usr/bin/env python3
"""Find const-string references to a given string in an APK.

This helps when a selector/device ID (e.g. "PO3") is present in the APK but
isn't passed as a direct const-string into the exact method we're scanning.

Usage:
  python3 tools/trace_apk_string_xrefs.py <apk> PO3 --context 20
"""

from __future__ import annotations

import argparse
import logging
from dataclasses import dataclass
from typing import Iterable

from androguard.core.analysis.analysis import Analysis
from androguard.misc import AnalyzeAPK


def _silence_loguru() -> None:
    try:
        from loguru import logger  # type: ignore

        logger.remove()
    except Exception:
        pass


@dataclass(frozen=True)
class Xref:
    cls: str
    method: str
    ins_index: int
    ins_output: str


def _iter_instructions(method_obj) -> Iterable:
    code = method_obj.get_code()
    if code is None:
        return
    bc = code.get_bc()
    for ins in bc.get_instructions():
        yield ins


def _format_method(m) -> str:
    return f"{m.get_class_name()}->{m.get_name()}{m.get_descriptor()}"


def main() -> None:
    logging.getLogger().setLevel(logging.WARNING)
    logging.getLogger("androguard").setLevel(logging.WARNING)
    _silence_loguru()

    ap = argparse.ArgumentParser()
    ap.add_argument("apk", help="Path to APK")
    ap.add_argument("needle", help="Exact string to find in const-string")
    ap.add_argument("--context", type=int, default=18)
    ap.add_argument("--limit", type=int, default=80)
    ns = ap.parse_args()

    a, dex_list, dx = AnalyzeAPK(ns.apk)
    assert isinstance(dx, Analysis)

    hits: list[Xref] = []

    for dex in dex_list:
        for cls in dex.get_classes():
            cls_name = cls.get_name()
            if "com/ihealth" not in cls_name:
                continue

            for m in cls.get_methods():
                method_sig = _format_method(m)
                ins_list = list(_iter_instructions(m))
                for idx, ins in enumerate(ins_list):
                    if ins.get_name() not in {"const-string", "const-string/jumbo"}:
                        continue
                    out = ins.get_output()
                    if f'"{ns.needle}"' not in out:
                        continue
                    hits.append(Xref(cls=cls_name, method=method_sig, ins_index=idx, ins_output=out))
                    if len(hits) >= ns.limit:
                        break
                if len(hits) >= ns.limit:
                    break
            if len(hits) >= ns.limit:
                break

    print(f"xrefs={len(hits)} needle={ns.needle!r}")

    if not hits:
        return

    # Second pass to print context
    wanted = {(h.cls, h.method, h.ins_index) for h in hits}

    for dex in dex_list:
        for cls in dex.get_classes():
            cls_name = cls.get_name()
            if "com/ihealth" not in cls_name:
                continue
            for m in cls.get_methods():
                method_sig = _format_method(m)
                ins_list = list(_iter_instructions(m))
                for idx, ins in enumerate(ins_list):
                    if (cls_name, method_sig, idx) not in wanted:
                        continue
                    print("=" * 120)
                    print(f"CLS  {cls_name}")
                    print(f"METH {method_sig}")
                    lo = max(0, idx - ns.context)
                    hi = min(len(ins_list), idx + ns.context + 1)
                    for j in range(lo, hi):
                        marker = ">>>" if j == idx else "   "
                        print(f"{marker} {j:5d} {ins_list[j].get_name():22s} {ins_list[j].get_output()}")


if __name__ == "__main__":
    main()
