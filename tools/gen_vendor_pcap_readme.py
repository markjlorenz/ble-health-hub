#!/usr/bin/env python3
"""Generate a Wireshark-like vendor message timeline for each capture.

This reads all `data/*.pcapng` files, finds occurrences of a vendor stage-1
handshake write from the central, then prints subsequent vendor messages (ATT
writes + notifications) with timestamps and direction.

It uses tshark in Docker (cincan/tshark) to avoid local Wireshark dependencies.

Usage:
  python3 tools/gen_vendor_pcap_readme.py > VENDOR_PCAP_LOGS_README.markdown

Notes:
- Time is reported as seconds since the matching central write per run.
- Directions are shown as: 🟦C→P (central write) and 🟩P→C (peripheral notify/indicate)
"""

from __future__ import annotations

import glob
import os
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable, List


START_HEX = "b0111101acfaf4b5c42f174e7d1b7a5ab654c0ef".lower()
START_PREFIX_HEX = "b0111101ac"  # B0 11 11 01 AC … (stage-1 write prefix)

# ATT opcodes of interest
OP_WRITE_REQ = "0x12"  # Write Request
OP_WRITE_CMD = "0x52"  # Write Command (Write Without Response)
OP_NOTIFY = "0x1b"  # Notification
OP_INDICATE = "0x1d"  # Indication (rare, but treat as peripheral->central)


def docker_base_args() -> List[str]:
    # Keep the command explicit and reproducible.
    # On Apple Silicon, use linux/amd64 to match the published image.
    return [
        "docker",
        "run",
        "--platform",
        "linux/amd64",
        "--rm",
        "-v",
        f"{os.getcwd()}:/work",
        "-w",
        "/work",
        "cincan/tshark",
    ]


@dataclass(frozen=True)
class AttRow:
    t_rel: float
    frame_no: int
    opcode: str
    value_hex: str
    access_address: str

    @property
    def direction_glyph(self) -> str:
        if self.opcode in (OP_WRITE_REQ, OP_WRITE_CMD):
            return "🟦C→P"
        if self.opcode in (OP_NOTIFY, OP_INDICATE):
            return "🟩P→C"
        return ""

    @property
    def is_vendor_value(self) -> bool:
        v = self.value_hex
        return v.startswith("a0") or v.startswith("b0")


def run_tshark_rows(pcap_path: str) -> List[AttRow]:
    # Filter to the exact set of opcodes we care about.
    display_filter = (
        f"btatt.opcode=={OP_WRITE_CMD} || btatt.opcode=={OP_WRITE_REQ} || "
        f"btatt.opcode=={OP_NOTIFY} || btatt.opcode=={OP_INDICATE}"
    )

    cmd = (
        docker_base_args()
        + [
            "-r",
            pcap_path,
            "-Y",
            display_filter,
            "-T",
            "fields",
            "-E",
            "separator=\t",
            "-e",
            "frame.time_relative",
            "-e",
            "frame.number",
            "-e",
            "btatt.opcode",
            "-e",
            "btatt.value",
            "-e",
            "btle.access_address",
        ]
    )

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"tshark failed for {pcap_path}:\n{proc.stderr.strip() or proc.stdout.strip()}"
        )

    rows: List[AttRow] = []
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        t_s, frame_s, opcode, value, access_address = (p.strip() for p in parts[:5])
        if not (t_s and frame_s and opcode and value):
            continue
        try:
            t_rel = float(t_s)
            frame_no = int(frame_s)
        except ValueError:
            continue
        rows.append(
            AttRow(
                t_rel=t_rel,
                frame_no=frame_no,
                opcode=opcode.lower(),
                value_hex=value.replace(":", "").lower(),
                access_address=access_address.lower() if access_address else "",
            )
        )

    return rows


def spaced_hex(hex_str: str) -> str:
    s = hex_str.strip().lower()
    if len(s) % 2 != 0:
        return s
    return " ".join(s[i : i + 2] for i in range(0, len(s), 2))


def clamp_cell(text: str, width: int) -> str:
    if len(text) == width:
        return text
    if len(text) < width:
        return text + (" " * (width - len(text)))
    return text[: max(0, width - 1)] + "…"


def emit_table(rows: Iterable[AttRow], t0: float) -> str:
    # Fixed columns, similar to Wireshark.
    w_time = 12
    w_dir = 5

    out_lines: List[str] = []
    header = (
        clamp_cell("Time", w_time)
        + "  "
        + clamp_cell("Dir", w_dir)
        + "  "
        + "Value"
    )
    underline = (
        "-" * w_time + "  " + "-" * w_dir + "  " + "-" * 5
    )

    out_lines.append(header)
    out_lines.append(underline)

    for r in rows:
        t = r.t_rel - t0
        out_lines.append(
            clamp_cell(f"{t:0.6f}", w_time)
            + "  "
            + clamp_cell(r.direction_glyph, w_dir)
            + "  "
            + spaced_hex(r.value_hex)
        )

    return "\n".join(out_lines)


def lookup_crc_init(pcap_path: str, access_address: str) -> str:
    aa = access_address.strip().lower()
    if not aa:
        return ""

    cmd = (
        docker_base_args()
        + [
            "-r",
            pcap_path,
            "-Y",
            f"btle.link_layer_data.access_address=={aa} && btle.link_layer_data.crc_init",
            "-T",
            "fields",
            "-E",
            "separator=\t",
            "-e",
            "btle.link_layer_data.crc_init",
        ]
    )

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        return ""

    # First line is enough; CRCInit is a per-connection parameter.
    for line in proc.stdout.splitlines():
        val = line.strip()
        if val:
            return val.lower()
    return ""


def stage1_start_indices(rows: List[AttRow]) -> List[int]:
    indices: List[int] = []
    for i, r in enumerate(rows):
        if r.opcode not in (OP_WRITE_REQ, OP_WRITE_CMD):
            continue
        if r.value_hex.startswith(START_PREFIX_HEX):
            indices.append(i)
    return indices


def main() -> int:
    pcaps_all = sorted(glob.glob("data/*.pcapng"))
    pcaps = [p for p in pcaps_all if "v2026" not in os.path.basename(p).lower()]
    if not pcaps:
        print("No data/*.pcapng files found.", file=sys.stderr)
        return 2

    print("# Vendor App PCAP Timelines")
    print()
    print(
        "Generated from the capture files in `data/`. Each section starts at the first "
        "central stage-1 write (`b0 11 11 01 ac …`) and lists subsequent vendor messages. "
        f"(Exact example from one capture: `{START_HEX}`)"
    )
    print()
    print("Direction glyphs: 🟦C→P (central write), 🟩P→C (peripheral notify/indicate)")
    print()

    any_emitted = False
    skipped: List[str] = []

    for pcap in pcaps:
        rows = run_tshark_rows(pcap)

        starts = stage1_start_indices(rows)
        if not starts:
            skipped.append(pcap)
            continue

        print(f"## {pcap}")
        print()

        for run_idx, idx0 in enumerate(starts, start=1):
            idx1 = starts[run_idx] if run_idx < len(starts) else len(rows)
            run_rows = [r for r in rows[idx0:idx1] if r.is_vendor_value]
            if not run_rows:
                continue

            any_emitted = True
            start_row = rows[idx0]
            rel0 = start_row.t_rel
            aa = start_row.access_address
            crc_init = lookup_crc_init(pcap, aa)

            print(f"### Run {run_idx}")
            print()
            print(
                f"Start: frame {start_row.frame_no} at capture t={start_row.t_rel:0.6f}s "
                f"(shown below as t=0.000000). Matched start write: `{start_row.value_hex}`"
            )
            if aa or crc_init:
                meta = []
                if aa:
                    meta.append(f"AA={aa}")
                if crc_init:
                    meta.append(f"CRCInit={crc_init}")
                print(f"Conn: {'  '.join(meta)}")
            print()
            print("```text")
            print(emit_table(run_rows, rel0))
            print("```")
            print()

    if not any_emitted:
        print(
            "No capture contained a stage-1 start write (b0 11 11 01 ac …). "
            "If these are not vendor-app captures, adjust START_PREFIX_HEX in tools/gen_vendor_pcap_readme.py."
        )

    if skipped:
        print("## Skipped captures")
        print()
        print(
            "These capture files did not contain a central stage-1 write matching the prefix "
            f"`{START_PREFIX_HEX}...`, so no vendor timeline section was emitted for them:"
        )
        print()
        for p in skipped:
            print(f"- {p}")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
