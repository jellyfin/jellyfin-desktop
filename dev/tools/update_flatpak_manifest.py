#!/usr/bin/env python3
"""Update CEF URL and SHA256 in the Flatpak manifest from CEF_VERSION."""

import argparse
import logging
import pathlib
import sys

DEV_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(DEV_DIR))
from download_cef import (
    CEF_DOWNLOAD_BASE,
    CEF_VERSION_FILE,
    read_pinned_version,
    resolve_distribution,
)

log = logging.getLogger(__name__)
MANIFEST_PATH = DEV_DIR / "flatpak" / "org.jellyfin.JellyfinDesktop.yml"
URL_PREFIX = f"url: {CEF_DOWNLOAD_BASE}/cef_binary_"
URL_SUFFIX = "_linux64_minimal.tar.bz2"


def manifest_cef_version(manifest_text):
    """Return the CEF version embedded in the manifest's archive URL, or None."""
    for line in manifest_text.splitlines():
        stripped = line.strip()
        if stripped.startswith(URL_PREFIX) and stripped.endswith(URL_SUFFIX):
            return stripped[len(URL_PREFIX) : -len(URL_SUFFIX)]
    return None


def patch_manifest(manifest_text, url, sha256):
    """Return manifest text with the CEF url + adjacent sha256 line rewritten."""
    out = []
    lines = iter(manifest_text.splitlines())
    for line in lines:
        if not line.lstrip().startswith(URL_PREFIX):
            out.append(line)
            continue
        indent = line[: len(line) - len(line.lstrip())]
        next_line = next(lines, "")
        if not next_line.lstrip().startswith("sha256:"):
            raise RuntimeError(
                f"Expected a 'sha256:' line after the CEF url in {MANIFEST_PATH}"
            )
        out.append(f"{indent}url: {url}")
        out.append(f"{indent}sha256: {sha256}")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if manifest does not match CEF_VERSION; do not modify",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    version = read_pinned_version()
    if not version:
        raise RuntimeError(
            f"CEF_VERSION file not found or empty: {CEF_VERSION_FILE}"
        )

    manifest_text = MANIFEST_PATH.read_text()
    actual = manifest_cef_version(manifest_text)
    if actual is None:
        raise RuntimeError(
            f"No CEF archive URL found in manifest {MANIFEST_PATH} "
            f"(expected 'url: {URL_PREFIX}<version>{URL_SUFFIX}')"
        )

    if actual == version:
        log.info("Flatpak manifest matches CEF_VERSION (%s)", version)
        return

    if args.check:
        log.error(
            "Flatpak manifest CEF version %s does not match CEF_VERSION %s",
            actual,
            version,
        )
        sys.exit(1)

    dist = resolve_distribution(version, platform_id="linux64")
    log.info("Updating Flatpak manifest to %s", dist["url"])
    MANIFEST_PATH.write_text(
        patch_manifest(manifest_text, dist["url"], dist["sha256"])
    )


if __name__ == "__main__":
    main()
