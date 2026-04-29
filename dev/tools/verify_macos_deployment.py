#!/usr/bin/env python3
"""Verify a macOS app bundle does not accidentally target too-new macOS.

The CI runners can build on newer macOS/Xcode while the distributed app is
intended to support older systems. This script inspects Mach-O load commands in
an app bundle and fails if any executable/dylib/framework advertises a minimum
macOS version above the requested deployment target.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

MACHO_SUFFIXES = {"", ".dylib", ".so"}
MINOS_RE = re.compile(r"minos\s+(\d+)\.(\d+)(?:\.(\d+))?")


def parse_version(value: str) -> tuple[int, int, int]:
    parts = value.split(".")
    if not 1 <= len(parts) <= 3:
        raise argparse.ArgumentTypeError(f"invalid version: {value}")
    try:
        nums = [int(part) for part in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid version: {value}") from exc
    while len(nums) < 3:
        nums.append(0)
    return tuple(nums)  # type: ignore[return-value]


def is_candidate(path: pathlib.Path) -> bool:
    if path.is_dir() or path.is_symlink():
        return False
    if path.suffix in MACHO_SUFFIXES:
        return True
    return path.parent.name == "MacOS"


def file_kind(path: pathlib.Path) -> str:
    try:
        return subprocess.check_output(["file", "-b", str(path)], text=True).strip()
    except subprocess.CalledProcessError:
        return ""


def macho_minos(path: pathlib.Path) -> tuple[int, int, int] | None:
    kind = file_kind(path)
    if "Mach-O" not in kind:
        return None
    output = subprocess.check_output(["vtool", "-show-build", str(path)], text=True)
    matches = [parse_version(".".join(part for part in match if part)) for match in MINOS_RE.findall(output)]
    if not matches:
        return None
    return max(matches)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=pathlib.Path, help="Path to .app bundle")
    parser.add_argument("--target", default="12.0", type=parse_version)
    args = parser.parse_args()

    if not args.app.exists():
        parser.error(f"app bundle does not exist: {args.app}")

    failures: list[str] = []
    inspected = 0
    for path in sorted(args.app.rglob("*")):
        if not is_candidate(path):
            continue
        minos = macho_minos(path)
        if minos is None:
            continue
        inspected += 1
        rel = path.relative_to(args.app)
        version = ".".join(str(part) for part in minos).rstrip(".0")
        if minos > args.target:
            failures.append(f"{rel}: minos {version}")

    if failures:
        print(f"Found {len(failures)} Mach-O files above target {args.target[0]}.{args.target[1]}:")
        print("\n".join(failures))
        return 1
    print(f"OK: inspected {inspected} Mach-O files; all target <= {args.target[0]}.{args.target[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
