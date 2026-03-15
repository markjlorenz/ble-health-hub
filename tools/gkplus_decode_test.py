#!/usr/bin/env python3
"""GK+ decoder + test helper (APK-derived).

Key correction from APK reverse engineering (Keto-Mojo Classic 2.6.6):

- `0xdb` packets are used for *records count* (not glucose/ketone values).
- Actual glucose/ketone measurements (and their timestamps) are carried in
    9-byte record payloads parsed by the SDK's VivaChek record parser.

This tool currently supports:

- Decoding a single `0xdb` records-count notification payload from hex
    (including CRC16/MODBUS validation).
- Decoding a 9-byte record snippet (18 hex chars) into:
    timestamp, sample type, prandial tag, and value.

Examples:
    # Decode records-count response (P->C notify)
    python3 tools/gkplus_decode_test.py --hex 7b01200110dbaa00020129000307037d

    # Decode a 9-byte record snippet (time + value + flags + type)
    python3 tools/gkplus_decode_test.py --record 1a030e090f00490011
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


def parse_hex(s: str) -> bytes:
    s = s.strip().lower().replace(" ", "")
    if s.startswith("0x"):
        s = s[2:]
    return bytes.fromhex(s)


@dataclass(frozen=True)
class DecodedDbCount:
    cmd: int
    data_bytes: bytes
    count: int
    crc16: int
    crc_bytes_expected: bytes
    crc_bytes_actual: bytes
    crc_ok: bool


@dataclass(frozen=True)
class DecodedRecord:
    when_local: datetime
    sample_type_code: int
    sample_type: str
    prandial: str
    raw_base100: int
    value: float
    unit: str


def crc16_modbus(data: bytes, init: int = 0xFFFF) -> int:
    """CRC-16/MODBUS (poly=0xA001, init=0xFFFF)."""

    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
        crc &= 0xFFFF
    return crc


def crc_send_bytes_from_crc16(crc16: int) -> bytes:
    """Match APK behavior: 4 nibbles (bytes 0..15), pairs swapped.

    Kotlin/Java behavior (Crc16Utils):
      - format as "%04X" => e.g. "7303"
      - turn each hex digit into a byte 0..15 => [7,3,0,3]
      - swap pairs: [[7,3],[0,3]] -> [[0,3],[7,3]] => [0,3,7,3]
    """

    s = f"{crc16:04X}"
    n = [int(ch, 16) for ch in s]
    return bytes([n[2], n[3], n[0], n[1]])


def decode_db_records_count(payload: bytes) -> DecodedDbCount | None:
    """Decode the GK+ records-count notify (cmd=0xdb, dir P->C: ...DB AA...)."""

    if len(payload) < 16 or payload[0] != 0x7B or payload[-1] != 0x7D:
        return None
    cmd = payload[5]
    if cmd != 0xDB:
        return None

    # The SDK's CountParser / HexRecordsCountParser match packets starting with:
    #   7B01200110DBAA...
    if len(payload) < 12 or payload[6] != 0xAA:
        return None

    # Layout per CommandUtils.isCommandCorrect:
    # - bytes[0] == 0x7B
    # - bytes[-1] == 0x7D
    # - bytes[-5:-1] are 4 CRC bytes (each is a nibble 0..15)
    # - CRC computed over bytes[1:-5]
    if len(payload) < 1 + 1 + 4 + 1:
        return None

    crc_bytes_actual = payload[-5:-1]
    body = payload[1:-5]
    crc16 = crc16_modbus(body)
    crc_bytes_expected = crc_send_bytes_from_crc16(crc16)
    crc_ok = crc_bytes_actual == crc_bytes_expected

    # Data field begins after 9 bytes (matches SDK substring(18,...)).
    data_bytes = payload[9:-5]
    if len(data_bytes) != 2:
        return None

    count = data_bytes[0] * 100 + data_bytes[1]
    return DecodedDbCount(
        cmd=cmd,
        data_bytes=data_bytes,
        count=count,
        crc16=crc16,
        crc_bytes_expected=crc_bytes_expected,
        crc_bytes_actual=crc_bytes_actual,
        crc_ok=crc_ok,
    )


_SAMPLE_TYPE_BY_CODE: dict[int, str] = {
    0x11: "GLUCOSE",
    0x12: "GLUCOSE (or GLUCOSE_GKI if paired)",
    0x22: "GLUCOSE_QC",
    0x55: "KETONE",
    0x56: "KETONE_GKI",
    0x66: "KETONE_QC",
}


def decode_vivachek_record_snippet(record9: bytes, glucose_unit: str = "mg/dL") -> DecodedRecord | None:
    """Decode the 9-byte record payload used by the official SDK.

    Record layout (VivaCheckRecordsParser.extractRecord + VivaChekTimeParser):
      - bytes[0:5] : YY MM DD HH mm (YY is years since 2000)
      - bytes[5:7] : value encoded base-100 => raw = b5*100 + b6
      - byte[7]    : flags; high nibble is prandial (1=PRE, 2=POST, else GENERAL)
      - byte[8]    : sample type code (0x11,0x12,0x22,0x55,0x56,0x66)

    Value conversion (BloodValuesParser):
      - glucose raw is mg/dL; if glucose_unit=="mmol/L", value = raw/18
            - ketone value = raw/100 (mmol/L)
    """

    if len(record9) != 9:
        return None

    yy, mo, dd, hh, mi = record9[0], record9[1], record9[2], record9[3], record9[4]
    when_local = datetime(2000 + yy, mo, dd, hh, mi)

    raw = record9[5] * 100 + record9[6]
    prandial_nibble = (record9[7] >> 4) & 0x0F
    prandial = {1: "PRE", 2: "POST"}.get(prandial_nibble, "GENERAL")

    st_code = record9[8]
    st = _SAMPLE_TYPE_BY_CODE.get(st_code, f"UNKNOWN(0x{st_code:02x})")

    if st_code in {0x55, 0x56, 0x66}:
        value = raw / 100.0
        unit = "mmol/L"
    else:
        if glucose_unit.lower() in {"mmol", "mmol/l", "mmoll", "mmolperliter"}:
            value = raw / 18.0
            unit = "mmol/L"
        else:
            value = float(raw)
            unit = "mg/dL"

    return DecodedRecord(
        when_local=when_local,
        sample_type_code=st_code,
        sample_type=st,
        prandial=prandial,
        raw_base100=raw,
        value=value,
        unit=unit,
    )


def iter_payloads_from_pcap(pcap_path: Path) -> Iterable[str]:
    repo_root = Path(__file__).resolve().parents[1]

    # Use dockerized tshark (repo convention).
    cmd = [
        "docker",
        "run",
        "--rm",
        "--platform",
        "linux/amd64",
        "-v",
        f"{repo_root}:/ws",
        "-w",
        "/ws",
        "cincan/tshark",
        "-r",
        str(pcap_path),
        "-Y",
        "btatt.opcode==0x1b && btatt.value contains db:aa",
        "-T",
        "fields",
        "-e",
        "btatt.value",
    ]

    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError:
        raise RuntimeError("docker not found on PATH")
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"tshark-in-docker failed: {e.output}")

    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        yield line


def print_decoded_db(hex_payload: str) -> None:
    payload = parse_hex(hex_payload)
    d = decode_db_records_count(payload)
    if not d:
        print(f"payload not a recognized 0xdb records-count frame: {hex_payload}")
        return

    print(f"payload: {hex_payload}")
    print(f"  cmd=0x{d.cmd:02x} records_count={d.count}")
    print(
        "  crc16/modbus="
        f"0x{d.crc16:04x} send_bytes_expected={d.crc_bytes_expected.hex()} actual={d.crc_bytes_actual.hex()} ok={d.crc_ok}"
    )


def print_decoded_record(hex_record: str, glucose_unit: str) -> None:
    b = parse_hex(hex_record)
    if len(b) != 9:
        print(f"record must be 9 bytes (18 hex chars): got {len(b)} bytes")
        return
    r = decode_vivachek_record_snippet(b, glucose_unit=glucose_unit)
    if not r:
        print("failed to decode record")
        return
    print(f"record: {hex_record}")
    print(f"  when_local={r.when_local:%Y-%m-%d %H:%M}")
    print(f"  sample_type={r.sample_type} (code=0x{r.sample_type_code:02x}) prandial={r.prandial}")
    if r.unit == "mg/dL":
        print(f"  value={int(r.value)} {r.unit} (raw_base100={r.raw_base100})")
    else:
        print(f"  value={r.value:.3f} {r.unit} (raw_base100={r.raw_base100})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hex", action="append", help="0xdb records-count notify payload hex (can pass multiple)")
    ap.add_argument("--pcap", help="extract 0xdb payloads from pcap via dockerized tshark")
    ap.add_argument("--record", action="append", help="9-byte record snippet hex (18 hex chars)")
    ap.add_argument(
        "--glucose-unit",
        default="mg/dL",
        choices=["mg/dL", "mmol/L"],
        help="Interpret glucose records as mg/dL or mmol/L (ketone is always mmol/L)",
    )
    args = ap.parse_args()

    did_any = False

    hexes: list[str] = []
    if args.hex:
        hexes.extend(args.hex)
    if args.pcap:
        hexes.extend(iter_payloads_from_pcap(Path(args.pcap)))

    for i, h in enumerate(hexes):
        did_any = True
        if i:
            print()
        print_decoded_db(h)

    if args.record:
        for i, r in enumerate(args.record):
            did_any = True
            if hexes or i:
                print()
            print_decoded_record(r, glucose_unit=args.glucose_unit)

    if not did_any:
        ap.error("provide --hex/--pcap and/or --record")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
