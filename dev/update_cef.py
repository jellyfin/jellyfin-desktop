#!/usr/bin/env python3
"""Update CEF version: write CEF_VERSION and sync Flatpak manifest."""

import argparse
import logging
import pathlib
import subprocess
import sys

from download_cef import (
    CEF_VERSION_FILE,
    read_pinned_version,
    relpath,
    resolve_distribution,
)

log = logging.getLogger(__name__)

FLATPAK_SCRIPT = pathlib.Path(__file__).parent / "flatpak" / "update_cef.py"


def check():
    """Return True if CEF_VERSION matches latest stable and Flatpak manifest."""
    current = read_pinned_version()
    latest = resolve_distribution(platform_id="linux64")["cef_version"]
    if current != latest:
        log.info("CEF_VERSION out of date: %s (latest: %s)", current, latest)
        return False
    if (
        subprocess.run(
            [sys.executable, str(FLATPAK_SCRIPT), "--check"]
        ).returncode
        != 0
    ):
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "version",
        nargs="?",
        help="CEF version (default: latest stable)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if CEF_VERSION is stale or Flatpak manifest is out of sync",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    if args.check:
        if args.version:
            parser.error("--check does not take a version argument")
        if not check():
            sys.exit(1)
        return

    dist = resolve_distribution(args.version or None, platform_id="linux64")
    version = dist["cef_version"]
    if not args.version:
        log.info("Latest stable CEF: %s", version)

    if read_pinned_version() == version:
        log.info("%s already at %s", relpath(CEF_VERSION_FILE), version)
    else:
        CEF_VERSION_FILE.write_text(version + "\n")
        log.info("Wrote %s -> %s", relpath(CEF_VERSION_FILE), version)

    subprocess.run([sys.executable, str(FLATPAK_SCRIPT)], check=True)


if __name__ == "__main__":
    main()
