#!/usr/bin/env python3
"""Trace GenerateKap/IdentifyIns getKey/getKa call sites in an APK.

Why this exists:
- Android native libs in the APK (libiHealth.so) can't be executed directly on
  desktop Linux/macOS because they link against Bionic (libc.so).
- Our earlier selector scan only recovers selectors passed as const-strings.
  For PO3, the selector often flows through fields/moves or helper methods.

This tool prints call sites AND surrounding instruction context so we can see
how the selector argument is constructed.

Usage:
  python3 tools/trace_apk_generatekap_calls.py <apk> --grep PO3
  python3 tools/trace_apk_generatekap_calls.py <apk> --only-po3

Notes:
- This does not decompile; it disassembles DEX instructions via androguard.
- Output is intentionally verbose but bounded.
"""

from __future__ import annotations

import argparse
import logging
import re
from dataclasses import dataclass
from typing import Iterable, Optional

from androguard.core.analysis.analysis import Analysis
from androguard.misc import AnalyzeAPK


def _silence_loguru() -> None:
    try:
        from loguru import logger  # type: ignore

        logger.remove()
    except Exception:
        pass


TARGET_INVOKE_RE = re.compile(
    r"Lcom/ihealth/communication/ins/(?:GenerateKap|IdentifyIns2?|IdentifyIns);->"
    r"(?:getKa|getKey)\([^)]*\)[^\s,]+"
)


@dataclass(frozen=True)
class CallSite:
    cls: str
    method: str
    invoked: str
    # Numbered instruction index within the method iteration
    ins_index: int
    op: str
    output: str


def _iter_instructions(method_obj) -> Iterable:
    code = method_obj.get_code()
    if code is None:
        return
    bc = code.get_bc()
    for ins in bc.get_instructions():
        yield ins


def _format_method(m) -> str:
    return f"{m.get_class_name()}->{m.get_name()}{m.get_descriptor()}"


def _invoked_label(ins_output: str) -> Optional[str]:
    m = TARGET_INVOKE_RE.search(ins_output)
    if not m:
        return None
    return m.group(0)


def _looks_po3_related(class_name: str, method_sig: str) -> bool:
    s = (class_name + " " + method_sig).lower()
    return any(
        t in s
        for t in [
            "/po3/",
            "po3",
            "spo2",
            "oximeter",
            "oxygen",
            "pulseox",
            "pulse ox",
        ]
    )


def scan(apk_path: str, only_po3: bool, grep: Optional[str], limit: int) -> list[CallSite]:
    _silence_loguru()
    a, dex_list, dx = AnalyzeAPK(apk_path)
    assert isinstance(dx, Analysis)

    out: list[CallSite] = []
    grep_re = re.compile(grep) if grep else None

    for dex in dex_list:
        for cls in dex.get_classes():
            cls_name = cls.get_name()
            if "com/ihealth" not in cls_name:
                continue

            for m in cls.get_methods():
                method_sig = _format_method(m)
                if only_po3 and not _looks_po3_related(cls_name, method_sig):
                    continue

                for idx, ins in enumerate(_iter_instructions(m)):
                    op = ins.get_name()
                    if not op.startswith("invoke-"):
                        continue
                    ins_out = ins.get_output()
                    invoked = _invoked_label(ins_out)
                    if invoked is None:
                        continue
                    if grep_re and not (grep_re.search(cls_name) or grep_re.search(method_sig) or grep_re.search(ins_out)):
                        continue
                    out.append(
                        CallSite(
                            cls=cls_name,
                            method=method_sig,
                            invoked=invoked,
                            ins_index=idx,
                            op=op,
                            output=ins_out,
                        )
                    )
                    if len(out) >= limit:
                        return out

    return out


def dump_context(apk_path: str, sites: list[CallSite], context: int) -> None:
    _silence_loguru()
    a, dex_list, dx = AnalyzeAPK(apk_path)
    assert isinstance(dx, Analysis)

    # Index sites by (class,method,ins_index)
    wanted = {(s.cls, s.method, s.ins_index) for s in sites}

    for dex in dex_list:
        for cls in dex.get_classes():
            cls_name = cls.get_name()
            if "com/ihealth" not in cls_name:
                continue
            for m in cls.get_methods():
                method_sig = _format_method(m)
                # Fast skip: does this method appear in wanted?
                # (ins_index differs, so we check with any match)
                if not any(w[0] == cls_name and w[1] == method_sig for w in wanted):
                    continue

                ins_list = list(_iter_instructions(m))
                for idx, ins in enumerate(ins_list):
                    if (cls_name, method_sig, idx) not in wanted:
                        continue

                    ins_out = ins.get_output()
                    invoked = _invoked_label(ins_out) or "<unknown>"
                    print("=" * 120)
                    print(f"CALL {invoked}")
                    print(f"IN   {method_sig}")
                    print(f"CLS  {cls_name}")
                    lo = max(0, idx - context)
                    hi = min(len(ins_list), idx + context + 1)
                    for j in range(lo, hi):
                        marker = ">>>" if j == idx else "   "
                        print(f"{marker} {j:5d} {ins_list[j].get_name():22s} {ins_list[j].get_output()}")


def main() -> None:
    logging.getLogger().setLevel(logging.WARNING)
    logging.getLogger("androguard").setLevel(logging.WARNING)

    ap = argparse.ArgumentParser()
    ap.add_argument("apk", help="Path to APK")
    ap.add_argument("--only-po3", action="store_true", help="Only scan PO3/pulse-ox related classes/methods")
    ap.add_argument("--grep", help="Regex filter applied to class/method/invoke output")
    ap.add_argument("--context", type=int, default=30, help="Instructions before/after to print")
    ap.add_argument("--limit", type=int, default=80, help="Max call sites to print")
    ns = ap.parse_args()

    sites = scan(ns.apk, only_po3=ns.only_po3, grep=ns.grep, limit=ns.limit)
    print(f"callsites={len(sites)}")
    for s in sites[: min(len(sites), 20)]:
        print(f"  {s.invoked}\t{s.method}")

    if not sites:
        return

    print("\n-- context dumps --")
    dump_context(ns.apk, sites, context=ns.context)


if __name__ == "__main__":
    main()
