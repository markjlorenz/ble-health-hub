#!/usr/bin/env python3
"""Scan an APK for calls to GenerateKap.getKey(String) and print selector strings.

Usage:
  python3 tools/scan_apk_generatekap_selectors.py <apk>

This intentionally avoids decompilation. It inspects DEX bytecode and extracts
constant-string arguments passed to:
  Lcom/ihealth/communication/ins/GenerateKap;->getKey(Ljava/lang/String;)[B

Output is best-effort: if the argument isn't a direct const-string in the same
basic block, we may not recover it.
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
    # androguard uses loguru for its debug output.
    try:
        from loguru import logger  # type: ignore

        logger.remove()
    except Exception:
        pass


TARGET_INVOKE_RE = re.compile(
    r"(Lcom/ihealth/communication/ins/(?:GenerateKap|IdentifyIns2?|IdentifyIns);->"
    r"(?:getKa|getKey)\([^)]*\)[^\s,]+)"
)


@dataclass(frozen=True)
class Hit:
    caller: str
    method: str
    invoked: str
    selector: Optional[str]


@dataclass(frozen=True)
class Invoke:
    invoked: str
    regs: list[str]
    op: str


def _iter_instructions(method_obj) -> Iterable:
    # method_obj is androguard.core.bytecodes.dvm.EncodedMethod
    code = method_obj.get_code()
    if code is None:
        return
    bc = code.get_bc()
    for ins in bc.get_instructions():
        yield ins


def _format_method(m) -> str:
    return f"{m.get_class_name()}->{m.get_name()}{m.get_descriptor()}"


def _is_invoke_to_generatekap(ins) -> bool:
    op = ins.get_name()
    if not op.startswith("invoke-"):
        return False
    out = ins.get_output()
    return TARGET_INVOKE_RE.search(out) is not None


def _parse_const_string(ins) -> Optional[tuple[str, str]]:
    if ins.get_name() not in {"const-string", "const-string/jumbo"}:
        return None
    out = ins.get_output()
    # Typical: v6, "PO3"
    parts = [p.strip() for p in out.split(",", 1)]
    if len(parts) != 2:
        return None
    reg = parts[0]
    rhs = parts[1]
    first_quote = rhs.find('"')
    last_quote = rhs.rfind('"')
    if first_quote == -1 or last_quote <= first_quote:
        return None
    return reg, rhs[first_quote + 1 : last_quote]


def _parse_invoke(ins) -> Optional[tuple[str, list[str]]]:
    """Return invoke info for selected invocations."""
    if not _is_invoke_to_generatekap(ins):
        return None
    out = ins.get_output()
    op = ins.get_name()
    m = TARGET_INVOKE_RE.search(out)
    if not m:
        return None
    invoked = m.group(1)

    def _expand_range(spec: str) -> list[str]:
        # spec like: "v0 .. v3" or "p1 .. p4"
        m = re.match(r"^\s*([vp])(\d+)\s*\.\.\s*([vp])(\d+)\s*$", spec)
        if not m:
            return []
        a_kind, a_num, b_kind, b_num = m.group(1), int(m.group(2)), m.group(3), int(m.group(4))
        if a_kind != b_kind or b_num < a_num:
            return []
        return [f"{a_kind}{i}" for i in range(a_num, b_num + 1)]

    def _parse_reglist(reglist: str) -> list[str]:
        reglist = reglist.strip()
        if not reglist:
            return []
        if ".." in reglist:
            expanded = _expand_range(reglist.replace("..", ".."))
            if expanded:
                return expanded
        regs_out: list[str] = []
        for tok in [t.strip() for t in reglist.split(",") if t.strip()]:
            if tok and tok[0] in {"v", "p"} and tok[1:].isdigit():
                regs_out.append(tok)
        return regs_out

    regs: list[str] = []
    # Most androguard outputs include a register list either as:
    #   "v0, v6, L...->method" OR "{v0, v6}, L...->method" OR "{v0 .. v3}, ..."
    if "{" in out and "}" in out:
        brace_start = out.find("{")
        brace_stop = out.find("}", brace_start + 1)
        if brace_stop != -1:
            regs = _parse_reglist(out[brace_start + 1 : brace_stop])
    if not regs:
        # Fallback: "v0, v6, Lcom/.../GenerateKap;->getKa(Ljava/lang/String;)[B"
        tokens = [t.strip() for t in out.split(",")]
        for t in tokens:
            if t.startswith("L") and "->" in t:
                break
            if t and (t[0] in {"v", "p"}) and t[1:].isdigit():
                regs.append(t)

    return Invoke(invoked=invoked, regs=regs, op=op)


def _parse_move(ins) -> Optional[tuple[str, str]]:
    op = ins.get_name()
    if op not in {
        "move-object",
        "move-object/from16",
        "move-object/16",
        "move",
        "move/from16",
        "move/16",
    }:
        return None
    out = ins.get_output()
    # Typical: "v1, v2"
    parts = [p.strip() for p in out.split(",")]
    if len(parts) < 2:
        return None
    dst = parts[0]
    src = parts[1]
    if not (dst and src):
        return None
    if dst[0] not in {"v", "p"} or not dst[1:].isdigit():
        return None
    if src[0] not in {"v", "p"} or not src[1:].isdigit():
        return None
    return dst, src


def scan(apk_path: str) -> list[Hit]:
    _silence_loguru()
    a, dex_list, dx = AnalyzeAPK(apk_path)
    assert isinstance(dx, Analysis)

    hits: list[Hit] = []

    for dex in dex_list:
        for cls in dex.get_classes():
            cls_name = cls.get_name()
            if "com/ihealth" not in cls_name:
                continue

            for m in cls.get_methods():
                recent_const: dict[str, str] = {}
                recent_order: list[str] = []
                for ins in _iter_instructions(m):
                    mv = _parse_move(ins)
                    if mv is not None:
                        dst, src = mv
                        if src in recent_const:
                            recent_const[dst] = recent_const[src]
                            recent_order.append(dst)

                    cs = _parse_const_string(ins)
                    if cs is not None:
                        reg, s = cs
                        recent_const[reg] = s
                        recent_order.append(reg)
                        if len(recent_order) > 200:
                            old = recent_order.pop(0)
                            # only delete if it's not re-added later
                            if old in recent_const and old not in recent_order:
                                recent_const.pop(old, None)

                    inv = _parse_invoke(ins)
                    if inv is not None:
                        invoked = inv.invoked
                        regs = inv.regs
                        # instance call: regs[0] = receiver, regs[1] = first arg
                        # static call: regs[0] = first arg
                        selector = None
                        arg_reg: Optional[str] = None
                        if inv.op.startswith("invoke-static"):
                            if len(regs) >= 1:
                                arg_reg = regs[0]
                        else:
                            if len(regs) >= 2:
                                arg_reg = regs[1]
                            elif len(regs) == 1:
                                # some tooling omits the receiver; best-effort
                                arg_reg = regs[0]

                        if arg_reg is not None:
                            selector = recent_const.get(arg_reg)
                        hits.append(
                            Hit(
                                caller=cls_name,
                                method=_format_method(m),
                                invoked=invoked,
                                selector=selector,
                            )
                        )

    return hits


def main() -> None:
    _silence_loguru()
    logging.getLogger().setLevel(logging.WARNING)
    logging.getLogger("androguard").setLevel(logging.WARNING)

    parser = argparse.ArgumentParser()
    parser.add_argument("apk", help="Path to APK")
    args = parser.parse_args()

    hits = scan(args.apk)

    # Summaries
    selectors = sorted({h.selector for h in hits if h.selector})
    unresolved = sum(1 for h in hits if h.selector is None)

    print(f"hits={len(hits)} selectors={len(selectors)} unresolved={unresolved}")
    if selectors:
        print("\nSelectors:")
        for s in selectors:
            print(f"  {s}")

    print("\nCall sites (first 120):")
    for h in hits[:120]:
        sel = h.selector if h.selector is not None else "<non-const>"
        print(f"  {h.invoked}\t{sel}\t{h.method}")


if __name__ == "__main__":
    main()
