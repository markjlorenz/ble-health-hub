#!/usr/bin/env python3
"""Verify PO3 Identify (stage-2) response generation against a vendor PCAP.

This script:
  1) Reads a .pcapng capture (HCI snoop / ATT-level)
  2) Locates the vendor characteristic handle carrying B0/A0 frames
  3) Reassembles the device Identify challenge (opcode + 48-byte payload)
  4) Extracts the app's stage-2 Identify response payload (AC FC ...)
  5) Recomputes the response using the reverse-engineered IdentifyIns/XXTEA logic
     and compares bytes.

Usage:
  .venv/bin/python tools/verify_po3_identify_response.py data/pulse-ox-2.pcapng

Notes:
- Requires scapy (already used by tools/analyze_pcap2.py).
- This focuses on stage-2 (deciphering) because stage-1 includes randomness.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
import re
import subprocess
import sys

from scapy.all import PcapNgReader  # type: ignore


PO3_KEY_BLOB16 = bytes.fromhex("bf4b051142d270a3932d2daacca9bd1e")
IDENTIFY_F_BYTES = b"Ch/HQ4LzItYT42s="
IDENTIFY_CMD = 0xAC
IDENTIFY_STAGE2_MARKER = 0xFC


def _u32(x: int) -> int:
    return x & 0xFFFFFFFF


def identify_c16(b16: bytes) -> bytes:
    if len(b16) != 16:
        raise ValueError(f"IdentifyIns.c expects 16 bytes, got {len(b16)}")
    out = bytearray(16)
    for i in range(4):
        out[i] = b16[3 - i]
        out[i + 4] = b16[7 - i]
        out[i + 8] = b16[11 - i]
        out[i + 12] = b16[15 - i]
    return bytes(out)


def nibble_swap(data: bytes) -> bytes:
    return bytes((((b & 0x0F) << 4) | ((b & 0xF0) >> 4)) & 0xFF for b in data)


def bytes_to_u32be(data: bytes) -> list[int]:
    if len(data) % 4 != 0:
        raise ValueError(f"Expected multiple of 4 bytes, got {len(data)}")
    out: list[int] = []
    for i in range(0, len(data), 4):
        out.append(int.from_bytes(data[i : i + 4], "big", signed=False))
    return out


def u32be_to_bytes(words: list[int]) -> bytes:
    return b"".join(_u32(w).to_bytes(4, "big", signed=False) for w in words)


def xxtea_encrypt_bytes(data: bytes, key16: bytes) -> bytes:
    if len(key16) != 16:
        raise ValueError("XXTEA needs a 128-bits key")

    if len(data) < 8:
        # Matches Java: if n < 2 ints => return unchanged
        return bytes(data)

    v = bytes_to_u32be(data)
    k = bytes_to_u32be(key16)

    n = len(v)
    rounds = 52 // n + 6
    delta = 1640531527  # 0x61C88647

    sum_ = 0
    z = v[n - 1]
    while rounds > 0:
        rounds -= 1
        sum_ = _u32(sum_ - delta)
        e = (sum_ >> 2) & 3

        for p in range(0, n - 1):
            y = v[p + 1]
            mx = _u32(
                (((_u32(z >> 5) ^ _u32(y << 2)) + (_u32(y >> 3) ^ _u32(z << 4))) ^ ((_u32(y) ^ sum_) + (_u32(z) ^ k[(p & 3) ^ e])))
            )
            v[p] = _u32(v[p] + mx)
            z = v[p]

        y0 = v[0]
        mx_last = _u32(
            (((_u32(z >> 5) ^ _u32(y0 << 2)) + (_u32(y0 >> 3) ^ _u32(z << 4))) ^ ((_u32(y0) ^ sum_) + (_u32(z) ^ k[((n - 1) & 3) ^ e])))
        )
        v[n - 1] = _u32(v[n - 1] + mx_last)
        z = v[n - 1]

    return u32be_to_bytes(v)


def identify_get_ka_po3() -> bytes:
    swapped_key = nibble_swap(PO3_KEY_BLOB16)
    swapped_f = nibble_swap(IDENTIFY_F_BYTES)
    return xxtea_encrypt_bytes(swapped_key, swapped_f)


def identify_get_ka_from_blob(blob16: bytes) -> bytes:
    if len(blob16) != 16:
        raise ValueError("Expected 16-byte blob")
    swapped_key = nibble_swap(blob16)
    swapped_f = nibble_swap(IDENTIFY_F_BYTES)
    return xxtea_encrypt_bytes(swapped_key, swapped_f)


def identify_deciphering(challenge48: bytes) -> bytes:
    if len(challenge48) != 48:
        raise ValueError(f"deciphering expects 48 bytes, got {len(challenge48)}")

    d = challenge48[0:16]
    b = challenge48[16:32]
    c = challenge48[32:48]

    ka = identify_get_ka_po3()
    t0 = xxtea_encrypt_bytes(identify_c16(d), ka)

    # Java calls XXTEA.encrypt(c(b), t0) but discards the result
    _ = xxtea_encrypt_bytes(identify_c16(b), t0)

    t2 = xxtea_encrypt_bytes(identify_c16(c), t0)
    out16 = identify_c16(t2)

    return bytes([IDENTIFY_CMD, IDENTIFY_STAGE2_MARKER]) + out16


def identify_deciphering_with_blob(challenge48: bytes, blob16: bytes) -> bytes:
    if len(challenge48) != 48:
        raise ValueError(f"deciphering expects 48 bytes, got {len(challenge48)}")
    if len(blob16) != 16:
        raise ValueError("Expected 16-byte blob")

    d = challenge48[0:16]
    b = challenge48[16:32]
    c = challenge48[32:48]

    ka = identify_get_ka_from_blob(blob16)
    t0 = xxtea_encrypt_bytes(identify_c16(d), ka)
    _ = xxtea_encrypt_bytes(identify_c16(b), t0)
    t2 = xxtea_encrypt_bytes(identify_c16(c), t0)
    out16 = identify_c16(t2)
    return bytes([IDENTIFY_CMD, IDENTIFY_STAGE2_MARKER]) + out16


_BLOB_LINE_RE = re.compile(r"\bblob16=([0-9a-fA-F]{32})\b")


def extract_native_blobs16(so_path: str, start: str, stop: str) -> list[bytes]:
    """Call tools/extract_libihealth_getkey_blobs.py and parse blob16 hex."""

    tool = os.path.join(os.path.dirname(__file__), "extract_libihealth_getkey_blobs.py")
    try:
        out = subprocess.check_output(
            [
                sys.executable,
                tool,
                so_path,
                "--start",
                start,
                "--stop",
                stop,
                "--len",
                "16",
                "--max",
                "500",
            ],
            text=True,
            errors="replace",
        )
    except Exception:
        return []

    blobs: list[bytes] = []
    seen: set[bytes] = set()
    for line in out.splitlines():
        m = _BLOB_LINE_RE.search(line)
        if not m:
            continue
        b = bytes.fromhex(m.group(1))
        if b in seen:
            continue
        seen.add(b)
        blobs.append(b)
    return blobs


@dataclass
class AttEvent:
    kind: str  # WRITE | NOTIFY
    idx: int
    handle: int
    value: bytes


def scan_att(data: bytes) -> tuple[int | None, int | None]:
    """Scan for ATT PDU header at any offset up to 16 bytes in."""
    for off in range(min(len(data), 16)):
        op = data[off]
        if op in (0x52, 0x12, 0x1B):
            return off, op
    return None, None


def parse_att_events(pcap_path: str) -> list[AttEvent]:
    events: list[AttEvent] = []
    for idx, p in enumerate(PcapNgReader(pcap_path)):
        b = bytes(p)
        off, op = scan_att(b)
        if op is None or off is None:
            continue

        payload = b[off + 1 :]
        if len(payload) < 3:
            continue

        handle = int.from_bytes(payload[0:2], "little")
        value = bytes(payload[2:])

        if op == 0x1B:
            events.append(AttEvent("NOTIFY", idx, handle, value))
        elif op in (0x52, 0x12):
            events.append(AttEvent("WRITE", idx, handle, value))

    return events


def autodetect_vendor_handle(events: list[AttEvent]) -> int:
    # Heuristic: only count values that *validate* as vendor frames
    # (length byte + checksum). This avoids false positives where a random
    # payload happens to start with 0xB0/0xA0.
    stats: dict[int, dict[str, int]] = {}
    for e in events:
        for frame in extract_vendor_frames(e.value):
            h = stats.setdefault(e.handle, {"WRITE": 0, "NOTIFY": 0, "TOTAL": 0})
            h[e.kind] += 1
            h["TOTAL"] += 1

    if not stats:
        raise SystemExit(
            "No valid vendor frames found (length+checksum) — is this the right capture format?"
        )

    # Prefer a handle that has both writes and notifications.
    candidates = [
        (handle, s)
        for handle, s in stats.items()
        if s.get("WRITE", 0) > 0 and s.get("NOTIFY", 0) > 0
    ]
    if not candidates:
        candidates = list(stats.items())

    handle, _ = max(candidates, key=lambda kv: kv[1]["TOTAL"])
    return handle


def validate_vendor_frame(value: bytes, head: int) -> bool:
    if len(value) < 6:
        return False
    if value[0] != head:
        return False
    total_len = (value[1] & 0xFF) + 3
    if total_len != len(value):
        return False

    checksum = sum(value[2:-1]) & 0xFF
    return checksum == (value[-1] & 0xFF)


def extract_vendor_frames(value: bytes) -> list[bytes]:
    """Extract valid vendor frames from an ATT value.

    Some captures may contain concatenated vendor frames in one ATT write/notify
    (or may include extra bytes). We scan for 0xA0/0xB0 and validate via
    length+checksum.
    """

    frames: list[bytes] = []
    i = 0
    while i + 2 <= len(value):
        head = value[i]
        if head not in (0xA0, 0xB0):
            i += 1
            continue
        total_len = (value[i + 1] & 0xFF) + 3
        if total_len < 6 or total_len > 512:
            i += 1
            continue
        if i + total_len > len(value):
            i += 1
            continue
        frame = value[i : i + total_len]
        if validate_vendor_frame(frame, head):
            frames.append(frame)
            i += total_len
            continue
        i += 1
    return frames


def parse_b0_payloads_from_writes(writes: list[bytes]) -> list[tuple[int, bytes]]:
    """Return list of (write_index, payload_bytes)."""
    out: list[tuple[int, bytes]] = []
    i = 0
    while i < len(writes):
        v = writes[i]
        if not validate_vendor_frame(v, 0xB0):
            i += 1
            continue

        meta = v[2] & 0xFF
        seq = v[3] & 0xFF
        cmd = v[4] & 0xFF

        if meta == 0x00:
            payload = bytes([cmd]) + v[5:-1]
            out.append((i, payload))
            i += 1
            continue

        if meta >= 0xA0:
            i += 1
            continue

        total = (meta >> 4) + 1
        reverse_index = meta & 0x0F
        frag_index = total - reverse_index - 1
        if frag_index != 0:
            i += 1
            continue

        expected_seq = [(seq + 2 * j) & 0xFF for j in range(total)]
        parts: dict[int, bytes] = {}

        ok = True
        for j in range(total):
            if i + j >= len(writes):
                ok = False
                break
            w = writes[i + j]
            if not validate_vendor_frame(w, 0xB0):
                ok = False
                break
            if (w[4] & 0xFF) != cmd:
                ok = False
                break
            if (w[3] & 0xFF) != expected_seq[j]:
                ok = False
                break

            m = w[2] & 0xFF
            t = (m >> 4) + 1
            if t != total:
                ok = False
                break
            rev = m & 0x0F
            fi = total - rev - 1
            parts[fi] = w[5:-1]

        if not ok or len(parts) != total:
            i += 1
            continue

        rest = b"".join(parts[j] for j in range(total))
        payload = bytes([cmd]) + rest
        out.append((i, payload))
        i += total

    return out


@dataclass
class IdentifyChallenge:
    opcode: int
    payload48: bytes
    first_pkt: int


def find_identify_challenge(notifs: list[tuple[int, bytes]]) -> IdentifyChallenge:
    """Reassemble the first 48-byte Identify challenge (A0 fragments, cmd=0xAC)."""

    session = None

    for pkt_idx, v in notifs:
        if not validate_vendor_frame(v, 0xA0):
            continue

        meta = v[2] & 0xFF
        if meta == 0x00 or meta == 0xF0 or meta >= 0xA0:
            continue

        if len(v) < 7:
            continue

        seq = v[3] & 0xFF
        cmd = v[4] & 0xFF
        if cmd != IDENTIFY_CMD:
            continue

        total = (meta >> 4) + 1
        reverse_index = meta & 0x0F
        frag_index = total - reverse_index - 1

        if session is None or session["total"] != total:
            session = {
                "total": total,
                "opcode": None,
                "expected": None,
                "parts": {},
                "first_pkt": pkt_idx,
            }

        if session["expected"] is None:
            # Matches BleUnPackageData.unPackageData(): expected seq table around current fragment
            expected = [0] * total
            for i in range(total):
                expected[i] = (seq + (i - frag_index) * 2) & 0xFF
            session["expected"] = expected

        if reverse_index == total - 1:
            session["opcode"] = v[5] & 0xFF
            payload = v[6:-1]
        else:
            payload = v[5:-1]

        session["parts"][seq] = payload

        if len(session["parts"]) != total or session["opcode"] is None:
            continue

        expected = session["expected"]
        assembled = b"".join(session["parts"].get(s, b"") for s in expected)

        if len(assembled) == 48:
            return IdentifyChallenge(
                opcode=int(session["opcode"]),
                payload48=assembled,
                first_pkt=int(session["first_pkt"]),
            )

        # Not the one we want; reset and keep scanning.
        session = None

    raise SystemExit("Failed to locate a 48-byte Identify challenge in notifications")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap", help="Path to .pcapng (e.g. data/pulse-ox-2.pcapng)")
    ap.add_argument(
        "--key-blob16",
        default=PO3_KEY_BLOB16.hex(),
        help="16-byte hex key blob used for the primary compute/compare step",
    )
    ap.add_argument(
        "--so",
        default="vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so",
        help="Path to libiHealth (for blob brute-force on mismatch)",
    )
    ap.add_argument(
        "--blob-scan-start",
        default="0x1f00",
        help="Start address for native getKey blob scan (hex)",
    )
    ap.add_argument(
        "--blob-scan-stop",
        default="0x2600",
        help="Stop address for native getKey blob scan (hex)",
    )
    ns = ap.parse_args()

    # Robust path: scan raw packet bytes for valid vendor frames (A0/B0) using
    # vendor length+checksum validation. This works even when ATT handle parsing
    # is unreliable due to capture format/DLT differences.
    writes: list[bytes] = []
    notifs: list[tuple[int, bytes]] = []
    for idx, p in enumerate(PcapNgReader(ns.pcap)):
        b = bytes(p)
        for frame in extract_vendor_frames(b):
            if frame[0] == 0xB0:
                writes.append(frame)
            elif frame[0] == 0xA0:
                notifs.append((idx, frame))

    if not writes:
        raise SystemExit("No valid B0 vendor frames found in capture")
    if not notifs:
        raise SystemExit("No valid A0 vendor frames found in capture")

    payloads = parse_b0_payloads_from_writes(writes)
    stage2_candidates = [
        (start_i, p)
        for (start_i, p) in payloads
        if len(p) == 18 and p[0] == IDENTIFY_CMD and p[1] == IDENTIFY_STAGE2_MARKER
    ]
    if not stage2_candidates:
        raise SystemExit("Failed to locate stage-2 response payload (AC FC ...)")

    stage2_write_i, captured_response = stage2_candidates[0]
    challenge = find_identify_challenge(notifs)

    try:
        key_blob = bytes.fromhex(ns.key_blob16)
    except Exception as e:
        raise SystemExit(f"Invalid --key-blob16 hex: {e}")
    computed_response = identify_deciphering_with_blob(challenge.payload48, key_blob)

    print(f"PCAP: {ns.pcap}")
    print(f"Scanned vendor frames (raw): writes={len(writes)} notifs={len(notifs)}")
    print(f"Identify challenge: opcode=0x{challenge.opcode:02x} first_pkt={challenge.first_pkt} payload48={challenge.payload48.hex()}")
    print(f"Captured stage2 payload: write_index={stage2_write_i} payload={captured_response.hex()}")
    print(f"Computed stage2 payload:               payload={computed_response.hex()}")

    if computed_response == captured_response:
        print("✅ MATCH: computed stage-2 response equals capture")
    else:
        print("❌ MISMATCH: computed stage-2 response differs")
        # Compact diff
        diffs = []
        for i, (a, b) in enumerate(zip(computed_response, captured_response)):
            if a != b:
                diffs.append((i, a, b))
        print(f"Diff bytes: {len(diffs)}")
        for i, a, b in diffs[:16]:
            print(f"  offset {i:02d}: computed={a:02x} captured={b:02x}")

        # Brute-force: try all 16-byte blobs referenced by native getKey and
        # see if any reproduces the capture.
        blobs = extract_native_blobs16(ns.so, ns.blob_scan_start, ns.blob_scan_stop)
        if not blobs:
            print("(blob brute-force unavailable: failed to extract native blobs)")
            return

        print(f"\nBrute-force: trying {len(blobs)} native blob16 candidates…")
        for blob in blobs:
            trial = identify_deciphering_with_blob(challenge.payload48, blob)
            if trial == captured_response:
                print(f"✅ Found matching native blob16: {blob.hex()}")
                return

        print("❌ No native blob16 candidate reproduced the captured response")


if __name__ == "__main__":
    main()
