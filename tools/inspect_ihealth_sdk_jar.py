#!/usr/bin/env python3
"""Inspect iHealth Android SDK jars without a Java runtime.

This script is intentionally "dumb": it does not decompile bytecode.
Instead it:
- parses classfile constant-pools to extract UTF8 strings
- scans raw .class bytes for known protocol frame byte sequences
- reports classes that reference crypto/RNG APIs

Typical usage:
  python3 tools/inspect_ihealth_sdk_jar.py /tmp/ihealth_sdk/iHealthLibrary_2.2.3.jar \
    --find-bytes "b0 11 11 01 ac" --find-bytes "b0 06 10 03 ac" \
    --keyword SecureRandom --keyword MessageDigest --keyword javax/crypto
"""

from __future__ import annotations

import argparse
import binascii
import re
import struct
import sys
import zipfile
from dataclasses import dataclass
from typing import Iterable, Iterator


MAGIC = b"\xCA\xFE\xBA\xBE"


@dataclass(frozen=True)
class ClassScanResult:
    entry: str
    size: int
    matched_bytes: list[bytes]
    matched_keywords: list[str]


def _parse_hex_bytes(spec: str) -> bytes:
    spec = spec.strip().replace(",", " ")
    parts = [p for p in spec.split() if p]
    try:
        return bytes(int(p, 16) for p in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"Invalid hex byte spec: {spec!r}") from exc


def iter_jar_class_entries(jar_path: str) -> Iterator[str]:
    with zipfile.ZipFile(jar_path) as zf:
        for name in zf.namelist():
            if name.endswith(".class"):
                yield name


def read_jar_entry(jar_path: str, entry: str) -> bytes:
    with zipfile.ZipFile(jar_path) as zf:
        return zf.read(entry)


def extract_constant_pool_utf8_strings(class_bytes: bytes) -> list[str]:
    if len(class_bytes) < 10 or class_bytes[:4] != MAGIC:
        return []

    cp_count = struct.unpack(">H", class_bytes[8:10])[0]
    offset = 10
    utf8_strings: list[str] = []

    # Constant pool index starts at 1; index 0 is invalid.
    i = 1
    while i < cp_count:
        if offset >= len(class_bytes):
            break

        tag = class_bytes[offset]
        offset += 1

        if tag == 1:  # CONSTANT_Utf8
            if offset + 2 > len(class_bytes):
                break
            length = struct.unpack(">H", class_bytes[offset : offset + 2])[0]
            offset += 2
            if offset + length > len(class_bytes):
                break
            s = class_bytes[offset : offset + length].decode("utf-8", "replace")
            utf8_strings.append(s)
            offset += length
        elif tag in (3, 4):  # Integer, Float
            offset += 4
        elif tag in (5, 6):  # Long, Double (takes two entries)
            offset += 8
            i += 1
        elif tag in (7, 8, 16):  # Class, String, MethodType
            offset += 2
        elif tag in (9, 10, 11, 12, 18):  # Fieldref/Methodref/InterfaceMethodref/NameAndType/InvokeDynamic
            offset += 4
        elif tag == 15:  # MethodHandle
            offset += 3
        else:
            # Unknown tag (newer classfile formats) - bail.
            break

        i += 1

    return utf8_strings


def _parse_constant_pool_utf8_by_index(class_bytes: bytes) -> tuple[dict[int, str], int]:
    """Return (utf8_by_index, offset_after_cp)."""
    if len(class_bytes) < 10 or class_bytes[:4] != MAGIC:
        return {}, 0

    cp_count = struct.unpack(">H", class_bytes[8:10])[0]
    offset = 10
    utf8_by_index: dict[int, str] = {}

    i = 1
    while i < cp_count:
        if offset >= len(class_bytes):
            break
        tag = class_bytes[offset]
        offset += 1
        if tag == 1:  # Utf8
            if offset + 2 > len(class_bytes):
                break
            length = struct.unpack(">H", class_bytes[offset : offset + 2])[0]
            offset += 2
            if offset + length > len(class_bytes):
                break
            utf8_by_index[i] = class_bytes[offset : offset + length].decode(
                "utf-8", "replace"
            )
            offset += length
        elif tag in (3, 4):
            offset += 4
        elif tag in (5, 6):
            offset += 8
            i += 1
        elif tag in (7, 8, 16):
            offset += 2
        elif tag in (9, 10, 11, 12, 18):
            offset += 4
        elif tag == 15:
            offset += 3
        else:
            break
        i += 1

    return utf8_by_index, offset


def dump_methods(class_bytes: bytes) -> list[tuple[str, str, int, bool]]:
    """Return list of (name, descriptor, access_flags, has_code_attr)."""
    utf8_by_index, offset = _parse_constant_pool_utf8_by_index(class_bytes)
    if not utf8_by_index or offset <= 0:
        return []

    # Skip: access_flags, this_class, super_class
    if offset + 6 > len(class_bytes):
        return []
    offset += 6

    # interfaces
    if offset + 2 > len(class_bytes):
        return []
    interfaces_count = struct.unpack(">H", class_bytes[offset : offset + 2])[0]
    offset += 2 + 2 * interfaces_count

    def skip_member_table(start: int) -> int:
        off = start
        if off + 2 > len(class_bytes):
            return off
        count = struct.unpack(">H", class_bytes[off : off + 2])[0]
        off += 2
        for _ in range(count):
            # access_flags, name_index, descriptor_index, attributes_count
            if off + 8 > len(class_bytes):
                return off
            off += 6
            attrs = struct.unpack(">H", class_bytes[off : off + 2])[0]
            off += 2
            for _ in range(attrs):
                if off + 6 > len(class_bytes):
                    return off
                _attr_name_index = struct.unpack(">H", class_bytes[off : off + 2])[0]
                attr_len = struct.unpack(">I", class_bytes[off + 2 : off + 6])[0]
                off += 6 + attr_len
        return off

    # fields
    offset = skip_member_table(offset)

    # methods
    if offset + 2 > len(class_bytes):
        return []
    methods_count = struct.unpack(">H", class_bytes[offset : offset + 2])[0]
    offset += 2

    out: list[tuple[str, str, int, bool]] = []
    for _ in range(methods_count):
        if offset + 8 > len(class_bytes):
            break
        access_flags = struct.unpack(">H", class_bytes[offset : offset + 2])[0]
        name_index = struct.unpack(">H", class_bytes[offset + 2 : offset + 4])[0]
        desc_index = struct.unpack(">H", class_bytes[offset + 4 : offset + 6])[0]
        attrs = struct.unpack(">H", class_bytes[offset + 6 : offset + 8])[0]
        offset += 8

        name = utf8_by_index.get(name_index, f"<cp:{name_index}>")
        desc = utf8_by_index.get(desc_index, f"<cp:{desc_index}>")

        has_code = False
        for _ in range(attrs):
            if offset + 6 > len(class_bytes):
                break
            attr_name_index = struct.unpack(">H", class_bytes[offset : offset + 2])[0]
            attr_len = struct.unpack(">I", class_bytes[offset + 2 : offset + 6])[0]
            attr_name = utf8_by_index.get(attr_name_index, "")
            if attr_name == "Code":
                has_code = True
            offset += 6 + attr_len

        out.append((name, desc, access_flags, has_code))

    return out


def scan_class(
    entry: str,
    class_bytes: bytes,
    find_bytes: list[bytes],
    keyword_patterns: list[re.Pattern[str]],
) -> ClassScanResult:
    matched_bytes: list[bytes] = [b for b in find_bytes if b and b in class_bytes]

    utf8_strings = extract_constant_pool_utf8_strings(class_bytes)
    matched_keywords: list[str] = []
    if keyword_patterns and utf8_strings:
        for pat in keyword_patterns:
            if any(pat.search(s) for s in utf8_strings):
                matched_keywords.append(pat.pattern)

    return ClassScanResult(
        entry=entry,
        size=len(class_bytes),
        matched_bytes=matched_bytes,
        matched_keywords=matched_keywords,
    )


def _human_hex(b: bytes) -> str:
    return " ".join(f"{x:02x}" for x in b)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("jar", help="Path to .jar file")
    ap.add_argument(
        "--find-bytes",
        action="append",
        default=[],
        metavar="HEX",
        help="Hex byte sequence to search for inside .class files (repeatable). Example: 'b0 11 11 01 ac'",
    )
    ap.add_argument(
        "--keyword",
        action="append",
        default=[],
        metavar="STR",
        help="Keyword/regex to search for inside constant-pool UTF8 strings (repeatable).",
    )
    ap.add_argument(
        "--max-results",
        type=int,
        default=200,
        help="Maximum number of matching classes to print.",
    )
    ap.add_argument(
        "--dump-utf8",
        action="append",
        default=[],
        metavar="CLASS",
        help=(
            "Dump ALL constant-pool UTF8 strings for a specific .class entry inside the jar "
            "(repeatable). Example: com/ihealth/communication/ins/x.class"
        ),
    )
    ap.add_argument(
        "--dump-filter",
        default=None,
        metavar="REGEX",
        help="Optional regex filter applied when dumping UTF8 strings (only prints matches).",
    )
    ap.add_argument(
        "--dump-methods",
        action="append",
        default=[],
        metavar="CLASS",
        help=(
            "Dump method names/descriptors + access flags for a specific .class entry inside the jar "
            "(repeatable). Example: com/ihealth/communication/ins/GenerateKap.class"
        ),
    )
    ap.add_argument(
        "--dump-methods-filter",
        default=None,
        metavar="REGEX",
        help=(
            "Optional regex filter applied when dumping methods (matches against name or descriptor). "
            "Useful for large classes."
        ),
    )

    ns = ap.parse_args(argv)

    find_bytes = [_parse_hex_bytes(s) for s in ns.find_bytes]
    keyword_patterns = [re.compile(k) for k in ns.keyword]

    dump_filter = re.compile(ns.dump_filter) if ns.dump_filter else None

    if ns.dump_utf8:
        with zipfile.ZipFile(ns.jar) as zf:
            for entry in ns.dump_utf8:
                data = zf.read(entry)
                utf8_strings = extract_constant_pool_utf8_strings(data)
                print(f"=== dump utf8: {entry} (size={len(data)})")
                for s in utf8_strings:
                    if dump_filter and not dump_filter.search(s):
                        continue
                    print(s)
        # Continue with the regular scan output too.

    if ns.dump_methods:
        methods_filter = re.compile(ns.dump_methods_filter) if ns.dump_methods_filter else None
        with zipfile.ZipFile(ns.jar) as zf:
            for entry in ns.dump_methods:
                data = zf.read(entry)
                methods = dump_methods(data)
                print(f"=== dump methods: {entry} (size={len(data)})")
                for name, desc, flags, has_code in methods:
                    if methods_filter and not (methods_filter.search(name) or methods_filter.search(desc)):
                        continue
                    is_native = bool(flags & 0x0100)
                    is_static = bool(flags & 0x0008)
                    print(
                        f"{name} {desc} flags=0x{flags:04x} "
                        f"native={int(is_native)} static={int(is_static)} code={int(has_code)}"
                    )

    matches: list[ClassScanResult] = []

    with zipfile.ZipFile(ns.jar) as zf:
        for entry in zf.namelist():
            if not entry.endswith(".class"):
                continue
            data = zf.read(entry)
            res = scan_class(entry, data, find_bytes=find_bytes, keyword_patterns=keyword_patterns)
            if res.matched_bytes or res.matched_keywords:
                matches.append(res)

    matches.sort(key=lambda r: (-(len(r.matched_bytes) + len(r.matched_keywords)), r.entry))

    print(f"jar: {ns.jar}")
    print(f"classes scanned: {len(list(iter_jar_class_entries(ns.jar)))}")
    print(f"matches: {len(matches)}")

    for res in matches[: ns.max_results]:
        parts: list[str] = []
        if res.matched_bytes:
            parts.append(
                "bytes="
                + ",".join(_human_hex(b) for b in res.matched_bytes)
            )
        if res.matched_keywords:
            parts.append("kw=" + ",".join(res.matched_keywords))
        print(f"- {res.entry} (size={res.size}) {(' '.join(parts)).strip()}")

    if len(matches) > ns.max_results:
        print(f"... truncated ({len(matches) - ns.max_results} more)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
