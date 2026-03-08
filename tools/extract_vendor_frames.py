#!/usr/bin/env python3
"""Extract iHealth vendor frames (A0/B0) from raw bytes in a .pcapng.

Why: Our earlier ATT-handle substring scans can over-capture bytes beyond the
actual value because they don't know the true ATT/L2CAP lengths. The vendor
protocol itself is self-delimiting:
  - Frame starts with 0xA0 (device->host) or 0xB0 (host->device)
  - Total length = frame[1] + 3
  - Checksum is 1 byte at the last index
  - Checksum rule (matches BleCommProtocol.b()/d()/e()):
      checksum == (sum(frame[2:len-1]) & 0xFF)

This script scans each packet's raw bytes for plausible frames and prints those
that validate.

Usage:
  .venv/bin/python tools/extract_vendor_frames.py data/pulse-ox.pcapng --max 80
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass

from scapy.all import PcapNgReader  # type: ignore


@dataclass(frozen=True)
class FrameHit:
    pkt: int
    off: int
    frame: bytes


def _checksum_ok(frame: bytes) -> bool:
    if len(frame) < 6:
        return False
    want = frame[-1]
    got = sum(frame[2:-1]) & 0xFF
    return want == got


def _iter_frames(raw: bytes):
    for off, b0 in enumerate(raw):
        if b0 not in (0xA0, 0xB0):
            continue
        if off + 2 >= len(raw):
            continue
        ln = raw[off + 1] + 3
        if ln < 6 or ln > 80:
            continue
        if off + ln > len(raw):
            continue
        frame = raw[off : off + ln]
        if _checksum_ok(frame):
            yield off, frame


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--max", type=int, default=120, help="Max frames to print")
    ns = ap.parse_args()

    hits: list[FrameHit] = []

    for pkt, p in enumerate(PcapNgReader(ns.pcap)):
        raw = bytes(p)
        for off, frame in _iter_frames(raw):
            hits.append(FrameHit(pkt=pkt, off=off, frame=frame))
            if len(hits) >= ns.max:
                break
        if len(hits) >= ns.max:
            break

    print(f"pcap={ns.pcap} hits={len(hits)}")
    for h in hits:
        print(
            f"pkt={h.pkt:5d} off={h.off:4d} len={len(h.frame):2d} "
            f"{h.frame.hex()} type={h.frame[0]:02x} cmd={h.frame[2]:02x} seq={h.frame[3]:02x}"
        )


if __name__ == "__main__":
    main()
