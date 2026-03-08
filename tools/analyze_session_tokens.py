#!/usr/bin/env python3
"""Analyze whether vendor stage-1 / b0 06 suffix looks derived from BLE CONNECT_IND parameters.

This script is intentionally conservative: it only checks for *trivial* derivations:
- direct byte matches (AA / CRCInit / window offset / interval / timeout / channel map)
- CRC32 and Adler32 over a few plausible concatenations

If none match, it doesn't prove cryptography, but it does rule out simple packing.

Usage:
  python3 tools/analyze_session_tokens.py data/pulse-ox-4-connect-twice.pcapng

Requires Docker (uses cincan/tshark).
"""

from __future__ import annotations

import os
import subprocess
import sys
import hashlib
import json
import zlib
from dataclasses import dataclass
from typing import Dict, List, Tuple, Optional

OP_WRITE_REQ = "0x12"
OP_WRITE_CMD = "0x52"

START_PREFIX_HEX = "b0111101ac"  # stage-1 write prefix
B006_PREFIX_HEX = "b0061003ac"  # the follow-up write we compare against


def docker_base_args() -> List[str]:
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


def run_tshark_fields(pcap_path: str, display_filter: str, fields: List[str]) -> List[List[str]]:
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
        ]
        + [x for f in fields for x in ("-e", f)]
    )

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip())

    rows: List[List[str]] = []
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        rows.append([p.strip() for p in line.split("\t")])
    return rows


def hex_to_bytes(hex_str: str) -> bytes:
    s = hex_str.replace(":", "").strip().lower()
    if s.startswith("0x"):
        s = s[2:]
    return bytes.fromhex(s)


@dataclass(frozen=True)
class ConnectParams:
    frame_no: int
    t_rel: float
    access_address: str
    crc_init: str
    window_size: int
    window_offset: int
    interval: int
    latency: int
    timeout: int
    channel_map_hex: str
    hop: int
    sca: int

    @property
    def aa_bytes_be(self) -> bytes:
        return hex_to_bytes(self.access_address)

    @property
    def aa_bytes_le(self) -> bytes:
        return self.aa_bytes_be[::-1]

    @property
    def crc_bytes_be(self) -> bytes:
        return hex_to_bytes(self.crc_init)

    @property
    def crc_bytes_le(self) -> bytes:
        return self.crc_bytes_be[::-1]

    @property
    def chmap_bytes(self) -> bytes:
        return hex_to_bytes(self.channel_map_hex)


@dataclass(frozen=True)
class RunTokens:
    stage1_hex: str
    b006_hex: str

    @property
    def stage1_bytes(self) -> bytes:
        return hex_to_bytes(self.stage1_hex)

    @property
    def stage1_tail14(self) -> bytes:
        # After: b0 11 11 01 ac fa (6 bytes)
        b = self.stage1_bytes
        return b[6:]

    @property
    def b006_suffix4(self) -> bytes:
        # After: b0 06 10 03 ac (5 bytes)
        b = self.b006_bytes
        return b[5:]

    @property
    def b006_bytes(self) -> bytes:
        return hex_to_bytes(self.b006_hex)


@dataclass(frozen=True)
class RunInfo:
    access_address: str
    stage1_frame_no: int
    stage1_t_rel: float
    tokens: RunTokens


def extract_connect_params(pcap_path: str) -> Dict[str, ConnectParams]:
    # CONNECT_IND is advertising pdu type 0x05 in this capture.
    filt = "btle.advertising_header.pdu_type==0x05 && btle.link_layer_data.access_address"
    fields = [
        "frame.number",
        "frame.time_relative",
        "btle.link_layer_data.access_address",
        "btle.link_layer_data.crc_init",
        "btle.link_layer_data.window_size",
        "btle.link_layer_data.window_offset",
        "btle.link_layer_data.interval",
        "btle.link_layer_data.latency",
        "btle.link_layer_data.timeout",
        "btle.link_layer_data.channel_map",
        "btle.link_layer_data.hop",
        "btle.link_layer_data.sleep_clock_accuracy",
    ]
    rows = run_tshark_fields(pcap_path, filt, fields)

    out: Dict[str, ConnectParams] = {}
    for r in rows:
        if len(r) < len(fields):
            continue
        (
            frame_no_s,
            t_s,
            aa,
            crc,
            win_sz,
            win_off,
            interval,
            latency,
            timeout,
            chmap,
            hop,
            sca,
        ) = r[: len(fields)]
        if not (frame_no_s and t_s and aa and crc):
            continue
        aa = aa.lower()
        out[aa] = ConnectParams(
            frame_no=int(frame_no_s),
            t_rel=float(t_s),
            access_address=aa,
            crc_init=crc.lower(),
            window_size=int(win_sz),
            window_offset=int(win_off),
            interval=int(interval),
            latency=int(latency),
            timeout=int(timeout),
            channel_map_hex=(chmap or "").strip().lower(),
            hop=int(hop) if hop else 0,
            sca=int(sca) if sca else 0,
        )
    return out


def extract_runs(pcap_path: str) -> List[Tuple[str, RunTokens]]:
    # Pull all ATT writes (write req/cmd) with btatt.value and access address
    filt = f"btatt.opcode=={OP_WRITE_CMD} || btatt.opcode=={OP_WRITE_REQ}"
    fields = [
        "frame.time_relative",
        "frame.number",
        "btatt.opcode",
        "btatt.value",
        "btle.access_address",
    ]
    rows = run_tshark_fields(pcap_path, filt, fields)

    # Normalize rows and find stage1 writes; for each stage1, find the first subsequent b0 06 10 03 ac write.
    writes: List[Tuple[float, int, str, str, str]] = []
    for r in rows:
        if len(r) < 5:
            continue
        t_s, frame_s, opcode, value, aa = r[:5]
        if not (t_s and frame_s and opcode and value):
            continue
        value_hex = value.replace(":", "").lower()
        writes.append((float(t_s), int(frame_s), opcode.lower(), value_hex, (aa or "").lower()))

    stage1_idxs = [i for i, (_t, _f, _op, v, _aa) in enumerate(writes) if v.startswith(START_PREFIX_HEX)]

    runs: List[RunInfo] = []
    for run_i, idx0 in enumerate(stage1_idxs):
        idx1 = stage1_idxs[run_i + 1] if run_i + 1 < len(stage1_idxs) else len(writes)
        stage1 = writes[idx0]
        stage1_t = stage1[0]
        stage1_frame = stage1[1]
        stage1_value = stage1[3]
        stage1_aa = stage1[4]

        b006_value = ""
        for j in range(idx0 + 1, idx1):
            v = writes[j][3]
            if v.startswith(B006_PREFIX_HEX):
                b006_value = v
                break

        if not b006_value:
            continue

        runs.append(
            RunInfo(
                access_address=stage1_aa,
                stage1_frame_no=stage1_frame,
                stage1_t_rel=stage1_t,
                tokens=RunTokens(stage1_hex=stage1_value, b006_hex=b006_value),
            )
        )

    return runs


def extract_ll_control_window_json_blob(
    pcap_path: str,
    access_address: str,
    t_start: float,
    t_end: float,
) -> Tuple[bytes, bytes, int]:
    """Return (canonical_json_blob_bytes, opcodes_bytes, packet_count)."""

    aa = access_address.strip().lower()
    if not aa:
        return b"", b"", 0

    # Use JSON output because this tshark build doesn't expose raw frame bytes.
    display_filter = (
        f"btle.access_address=={aa} && btle.control_opcode && "
        f"frame.time_relative>={t_start:.6f} && frame.time_relative<={t_end:.6f}"
    )

    cmd = docker_base_args() + ["-r", pcap_path, "-Y", display_filter, "-T", "json"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip())

    packets = json.loads(proc.stdout or "[]")
    opcodes: List[int] = []
    canon_lines: List[str] = []

    for pkt in packets:
        layers = pkt.get("_source", {}).get("layers", {})
        btle = layers.get("btle", {})

        opcode_val: Optional[str] = None
        # There are a few possible paths depending on tshark version/format.
        if isinstance(btle, dict):
            opcode_val = btle.get("btle.control_opcode")

        if opcode_val is None:
            # fallback: recursive scan for key
            def find_opcode(obj):
                if isinstance(obj, dict):
                    if "btle.control_opcode" in obj:
                        return obj["btle.control_opcode"]
                    for v in obj.values():
                        got = find_opcode(v)
                        if got is not None:
                            return got
                elif isinstance(obj, list):
                    for v in obj:
                        got = find_opcode(v)
                        if got is not None:
                            return got
                return None

            opcode_val = find_opcode(layers)

        if isinstance(opcode_val, list) and opcode_val:
            opcode_val = opcode_val[0]

        if isinstance(opcode_val, str):
            opcode_val = opcode_val.strip().lower()
            if opcode_val.startswith("0x"):
                try:
                    opcodes.append(int(opcode_val, 16) & 0xFF)
                except ValueError:
                    pass

        # Canonicalize a reduced view of layers to keep it stable-ish.
        # Exclude frame timing/noise; keep btle + btle_control + l2cap if present.
        reduced = {}
        for k in ("btle", "btle.control", "btle_rf", "l2cap", "btatt"):
            if k in layers:
                reduced[k] = layers[k]
        if not reduced:
            reduced = layers
        canon_lines.append(json.dumps(reduced, sort_keys=True, separators=(",", ":")))

    blob = ("\n".join(canon_lines)).encode("utf-8")
    return blob, bytes(opcodes), len(packets)


def crc32_bytes(data: bytes) -> bytes:
    return (zlib.crc32(data) & 0xFFFFFFFF).to_bytes(4, "big")


def adler32_bytes(data: bytes) -> bytes:
    return (zlib.adler32(data) & 0xFFFFFFFF).to_bytes(4, "big")


def fnv1a32_bytes(data: bytes) -> bytes:
    # FNV-1a 32-bit
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h.to_bytes(4, "big")


def jenkins_oaat32_bytes(data: bytes) -> bytes:
    # Jenkins one-at-a-time hash, 32-bit
    h = 0
    for b in data:
        h = (h + b) & 0xFFFFFFFF
        h = (h + ((h << 10) & 0xFFFFFFFF)) & 0xFFFFFFFF
        h ^= (h >> 6)
    h = (h + ((h << 3) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h ^= (h >> 11)
    h = (h + ((h << 15) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h.to_bytes(4, "big")


def digest_truncations(data: bytes) -> Dict[str, bytes]:
    out: Dict[str, bytes] = {}
    digests = {
        "md5": hashlib.md5(data).digest(),
        "sha1": hashlib.sha1(data).digest(),
        "sha256": hashlib.sha256(data).digest(),
        "blake2s": hashlib.blake2s(data).digest(),
        "blake2b": hashlib.blake2b(data).digest(),
    }
    for name, d in digests.items():
        out[f"{name}[:4]"] = d[:4]
        out[f"{name}[-4:]"] = d[-4:]
    return out


def xor_fold4(data: bytes) -> bytes:
    acc = [0, 0, 0, 0]
    for i, b in enumerate(data):
        acc[i % 4] ^= b
    return bytes(acc)


def sum32_bytes(data: bytes) -> bytes:
    s = sum(data) & 0xFFFFFFFF
    return s.to_bytes(4, "big")


def check_trivial_derivations(cp: ConnectParams, tokens: RunTokens) -> List[str]:
    target = tokens.b006_suffix4

    def u16le(n: int) -> bytes:
        return n.to_bytes(2, "little")

    def u16be(n: int) -> bytes:
        return n.to_bytes(2, "big")

    candidates: Dict[str, bytes] = {
        "AA(be)": cp.aa_bytes_be,
        "AA(le)": cp.aa_bytes_le,
        "CRCInit(be)": cp.crc_bytes_be,
        "CRCInit(le)": cp.crc_bytes_le,
        "WinOffset(u16le)": u16le(cp.window_offset),
        "WinOffset(u16be)": u16be(cp.window_offset),
        "Interval(u16le)": u16le(cp.interval),
        "Timeout(u16le)": u16le(cp.timeout),
        "ChMap(5B)": cp.chmap_bytes,
        "Stage1(full)": tokens.stage1_bytes,
        "Stage1(tail14)": tokens.stage1_tail14,
        "Stage1(tail14)[:4]": tokens.stage1_tail14[:4],
        "Stage1(tail14)[-4:]": tokens.stage1_tail14[-4:],
        "Stage1(full)[:4]": tokens.stage1_bytes[:4],
        "Stage1(full)[-4:]": tokens.stage1_bytes[-4:],
    }

    combos: Dict[str, bytes] = {
        "AA(le)||CRC(le)": cp.aa_bytes_le + cp.crc_bytes_le,
        "CRC(le)||AA(le)": cp.crc_bytes_le + cp.aa_bytes_le,
        "AA(be)||CRC(be)": cp.aa_bytes_be + cp.crc_bytes_be,
        "CONNECT(le-ish)": b"".join(
            [
                cp.aa_bytes_le,
                cp.crc_bytes_le,
                bytes([cp.window_size & 0xFF]),
                u16le(cp.window_offset),
                u16le(cp.interval),
                u16le(cp.latency),
                u16le(cp.timeout),
                cp.chmap_bytes,
                bytes([((cp.sca & 0x07) << 5) | (cp.hop & 0x1F)]),
            ]
        ),
        "CONNECT(spec-order,le-fields)": b"".join(
            [
                cp.aa_bytes_be,  # spec shows AA in that field as 32-bit; keep BE here as a separate hypothesis
                cp.crc_bytes_be,
                bytes([cp.window_size & 0xFF]),
                u16le(cp.window_offset),
                u16le(cp.interval),
                u16le(cp.latency),
                u16le(cp.timeout),
                cp.chmap_bytes,
                bytes([((cp.sca & 0x07) << 5) | (cp.hop & 0x1F)]),
            ]
        ),
    }

    # Combine stage-1 entropy with connection params (both orders).
    # If b006 suffix is a checksum/MAC-like value over stage1 + conn context, these are the first things to try.
    connect_blob = combos["CONNECT(le-ish)"]
    combos.update(
        {
            "Stage1(tail14)||CONNECT": tokens.stage1_tail14 + connect_blob,
            "CONNECT||Stage1(tail14)": connect_blob + tokens.stage1_tail14,
            "Stage1(full)||CONNECT": tokens.stage1_bytes + connect_blob,
            "CONNECT||Stage1(full)": connect_blob + tokens.stage1_bytes,
            "Stage1(tail14)||AA(le)": tokens.stage1_tail14 + cp.aa_bytes_le,
            "AA(le)||Stage1(tail14)": cp.aa_bytes_le + tokens.stage1_tail14,
            "Stage1(tail14)||CRC(le)": tokens.stage1_tail14 + cp.crc_bytes_le,
            "CRC(le)||Stage1(tail14)": cp.crc_bytes_le + tokens.stage1_tail14,
        }
    )

    hits: List[str] = []

    for label, data in {**candidates, **combos}.items():
        if data == target:
            hits.append(f"DIRECT: b006_suffix == {label}")

        # Contiguous 4-byte window search (slice-extraction hypothesis)
        if len(data) >= 4:
            for off in range(0, len(data) - 4 + 1):
                if data[off : off + 4] == target:
                    hits.append(f"SUBSTR4({label}) contains b006_suffix at offset {off}")
                    break

        if len(data) >= 4:
            xf = xor_fold4(data)
            sm = sum32_bytes(data)
            if xf == target:
                hits.append(f"XORFOLD4({label}) matches b006_suffix")
            if xf[::-1] == target:
                hits.append(f"XORFOLD4({label}) little-end bytes matches b006_suffix")
            if sm == target:
                hits.append(f"SUM32({label}) matches b006_suffix")
            if sm[::-1] == target:
                hits.append(f"SUM32({label}) little-end bytes matches b006_suffix")

        c32 = crc32_bytes(data)
        a32 = adler32_bytes(data)
        f32 = fnv1a32_bytes(data)
        j32 = jenkins_oaat32_bytes(data)
        if c32 == target:
            hits.append(f"CRC32({label}) matches b006_suffix")
        if c32[::-1] == target:
            hits.append(f"CRC32({label}) little-end bytes matches b006_suffix")
        if a32 == target:
            hits.append(f"Adler32({label}) matches b006_suffix")
        if a32[::-1] == target:
            hits.append(f"Adler32({label}) little-end bytes matches b006_suffix")
        if f32 == target:
            hits.append(f"FNV1a32({label}) matches b006_suffix")
        if f32[::-1] == target:
            hits.append(f"FNV1a32({label}) little-end bytes matches b006_suffix")
        if j32 == target:
            hits.append(f"JenkinsOAAT32({label}) matches b006_suffix")
        if j32[::-1] == target:
            hits.append(f"JenkinsOAAT32({label}) little-end bytes matches b006_suffix")

        for dlabel, dbytes in digest_truncations(data).items():
            if dbytes == target:
                hits.append(f"DIGEST({label}): {dlabel} matches b006_suffix")
            if dbytes[::-1] == target:
                hits.append(f"DIGEST({label}): {dlabel} little-end bytes matches b006_suffix")

    return hits


def check_against_ll_control(
    ll_blob: bytes,
    ll_opcodes: bytes,
    tokens: RunTokens,
) -> List[str]:
    target = tokens.b006_suffix4
    hits: List[str] = []

    candidates: Dict[str, bytes] = {
        "LL(json_blob)": ll_blob,
        "LL(opcodes)": ll_opcodes,
        "LL(json_blob)||Stage1(tail14)": ll_blob + tokens.stage1_tail14,
        "Stage1(tail14)||LL(json_blob)": tokens.stage1_tail14 + ll_blob,
        "LL(opcodes)||Stage1(tail14)": ll_opcodes + tokens.stage1_tail14,
        "Stage1(tail14)||LL(opcodes)": tokens.stage1_tail14 + ll_opcodes,
    }

    for label, data in candidates.items():
        # Contiguous 4-byte window search (slice-extraction hypothesis)
        if len(data) >= 4:
            for off in range(0, len(data) - 4 + 1):
                if data[off : off + 4] == target:
                    hits.append(f"SUBSTR4({label}) contains b006_suffix at offset {off}")
                    break

        # Try the same reducers/hashes we already use.
        xf = xor_fold4(data) if len(data) >= 4 else b""
        sm = sum32_bytes(data)
        if xf and (xf == target or xf[::-1] == target):
            hits.append(f"XORFOLD4({label}) matches b006_suffix")
        if sm == target or sm[::-1] == target:
            hits.append(f"SUM32({label}) matches b006_suffix")

        for name, fn in (
            ("CRC32", crc32_bytes),
            ("Adler32", adler32_bytes),
            ("FNV1a32", fnv1a32_bytes),
            ("JenkinsOAAT32", jenkins_oaat32_bytes),
        ):
            val = fn(data)
            if val == target:
                hits.append(f"{name}({label}) matches b006_suffix")
            if val[::-1] == target:
                hits.append(f"{name}({label}) little-end bytes matches b006_suffix")

        for dlabel, dbytes in digest_truncations(data).items():
            if dbytes == target:
                hits.append(f"DIGEST({label}): {dlabel} matches b006_suffix")
            if dbytes[::-1] == target:
                hits.append(f"DIGEST({label}): {dlabel} little-end bytes matches b006_suffix")

    return hits


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: python3 tools/analyze_session_tokens.py data/your.pcapng", file=sys.stderr)
        return 2

    pcap_path = sys.argv[1]
    if not os.path.exists(pcap_path):
        print(f"PCAP not found: {pcap_path}", file=sys.stderr)
        return 2

    connect = extract_connect_params(pcap_path)
    runs = extract_runs(pcap_path)

    print(f"PCAP: {pcap_path}")
    print(f"CONNECT_IND entries: {len(connect)}")
    print(f"Runs (stage1 occurrences w/ b006 follow-up): {len(runs)}")

    for i, run in enumerate(runs, start=1):
        aa = run.access_address
        tokens = run.tokens
        cp = connect.get(aa)
        print("\n" + ("=" * 60))
        print(f"Run {i}: AA={aa}")
        print(f"stage1 frame/time: {run.stage1_frame_no} @ {run.stage1_t_rel:0.6f}s")
        print(f"stage1: {tokens.stage1_hex}")
        print(f"b006  : {tokens.b006_hex}")
        print(f"b006 suffix4: {tokens.b006_suffix4.hex()}")
        print(f"stage1 tail14: {tokens.stage1_tail14.hex()}")

        if not cp:
            print("No CONNECT_IND params found for this AA")
            continue

        print(
            "CONNECT_IND params: "
            f"CRCInit={cp.crc_init} win_off={cp.window_offset} interval={cp.interval} "
            f"timeout={cp.timeout} chmap={cp.channel_map_hex} hop={cp.hop} sca={cp.sca}"
        )

        hits = check_trivial_derivations(cp, tokens)
        if hits:
            print("TRIVIAL MATCHES:")
            for h in hits:
                print("-", h)
        else:
            print(
                "No trivial match: b006 suffix != direct fields and != simple hashes "
                "(CRC32/Adler32/FNV1a/Jenkins, digest truncations, XORFOLD4, SUM32) "
                "of tested fields/concats"
            )

        # LL control window analysis
        try:
            ll_blob, ll_opcodes, ll_count = extract_ll_control_window_json_blob(
                pcap_path,
                aa,
                t_start=cp.t_rel,
                t_end=run.stage1_t_rel,
            )
        except Exception as e:
            print(f"LL control extract failed: {e}")
            continue

        print(f"LL control packets between CONNECT_IND and stage1: {ll_count}")
        if ll_opcodes:
            op_hex = " ".join(f"{b:02x}" for b in ll_opcodes)
            print(f"LL control opcodes ({len(ll_opcodes)}): {op_hex}")
        ll_hits = check_against_ll_control(ll_blob, ll_opcodes, tokens)
        if ll_hits:
            print("LL CONTROL MATCHES:")
            for h in ll_hits:
                print("-", h)
        else:
            print("No LL-control-based trivial match (same reducers/hashes as above)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
