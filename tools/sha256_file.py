#!/usr/bin/env python3
"""Print SHA256 of a file.

Usage:
  python3 tools/sha256_file.py "path/to/file"

This is a small helper so collaborators can verify large binaries (like vendor
APKs) without needing extra tooling.
"""

from __future__ import annotations

import argparse
import hashlib


def sha256_path(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ns = ap.parse_args()
    print(sha256_path(ns.path))


if __name__ == "__main__":
    main()
