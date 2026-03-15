#!/usr/bin/env python3
"""GK+ regression test vectors.

This checks that our decoding and *display* rules match the official app:

- Glucose shown: integer mg/dL
- Ketone shown: rounded to 1 decimal
- GKI shown: computed from shown values, then truncated to 1 decimal

Vectors live in tools/gkplus_regression_vectors.json and use the 9-byte
record snippets extracted from record-transfer frames.
"""

from __future__ import annotations

import json
import math
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
VECTORS_PATH = REPO_ROOT / "tools" / "gkplus_regression_vectors.json"
DECODER_PATH = REPO_ROOT / "tools" / "gkplus_decode_test.py"


def _load_decoder_module():
    import importlib.util

    spec = importlib.util.spec_from_file_location("gkplus_decode_test", DECODER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load decoder module from {DECODER_PATH}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def round1(x: float) -> float:
    # Matches JS Math.round for positive values.
    return math.floor(x * 10.0 + 0.5) / 10.0


def trunc1(x: float) -> float:
    return math.floor(x * 10.0) / 10.0


def gki_shown(glucose_mgdl: int, ketone_1dp: float) -> float:
    if ketone_1dp <= 0:
        raise ValueError("ketone must be > 0")
    g_mmol = glucose_mgdl / 18.0
    return trunc1(g_mmol / ketone_1dp)


def parse_when_local(s: str) -> datetime:
    return datetime.strptime(s, "%Y-%m-%d %H:%M")


@dataclass(frozen=True)
class CaseResult:
    case_id: str
    ok: bool
    message: str


def run_case(d, case: dict) -> CaseResult:
    case_id = str(case.get("id", "(missing id)"))
    glu9 = str(case.get("glu9", ""))
    ket9 = str(case.get("ket9", ""))
    exp = case.get("expected") or {}
    exp_when = parse_when_local(str(exp.get("when_local")))
    exp_glu = int(exp.get("glucose_mgdl"))
    exp_ket = float(exp.get("ketone_mmol_l_1dp"))
    exp_gki = float(exp.get("gki_1dp"))

    glu = d.decode_vivachek_record_snippet(d.parse_hex(glu9), glucose_unit="mg/dL")
    ket = d.decode_vivachek_record_snippet(d.parse_hex(ket9), glucose_unit="mg/dL")
    if not glu or not ket:
        return CaseResult(case_id, False, "failed to decode glu9/ket9")

    if glu.when_local != exp_when or ket.when_local != exp_when:
        return CaseResult(
            case_id,
            False,
            f"when mismatch: glu={glu.when_local:%Y-%m-%d %H:%M} ket={ket.when_local:%Y-%m-%d %H:%M} expected={exp_when:%Y-%m-%d %H:%M}",
        )

    glu_shown = int(round(float(glu.value)))
    ket_shown = round1(float(ket.value))
    gki = gki_shown(glu_shown, ket_shown)

    if glu_shown != exp_glu:
        return CaseResult(case_id, False, f"glucose mismatch: got={glu_shown} expected={exp_glu}")
    if abs(ket_shown - exp_ket) > 1e-9:
        return CaseResult(case_id, False, f"ketone mismatch: got={ket_shown:.1f} expected={exp_ket:.1f} (raw={ket.raw_base100})")
    if abs(gki - exp_gki) > 1e-9:
        return CaseResult(case_id, False, f"gki mismatch: got={gki:.1f} expected={exp_gki:.1f}")

    # Sanity: pairing window.
    dt_min = abs(int((glu.when_local - ket.when_local).total_seconds() // 60))
    if dt_min > 15:
        return CaseResult(case_id, False, f"pairing window exceeded: Δ={dt_min} minutes")

    return CaseResult(case_id, True, "ok")


def main() -> int:
    d = _load_decoder_module()
    cases = json.loads(VECTORS_PATH.read_text("utf-8"))
    if not isinstance(cases, list) or not cases:
        print(f"No cases found in {VECTORS_PATH}")
        return 2

    failures: list[CaseResult] = []
    for case in cases:
        r = run_case(d, case)
        if not r.ok:
            failures.append(r)

    if failures:
        print(f"FAIL ({len(failures)}/{len(cases)} failing)")
        for f in failures:
            print(f"- {f.case_id}: {f.message}")
        return 1

    print(f"OK ({len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
