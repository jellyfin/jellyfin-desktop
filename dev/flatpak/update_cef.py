#!/usr/bin/env python3
"""Update CEF URL and SHA256 in the Flatpak manifest from CEF_VERSION."""

import logging
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent.parent))
from download_cef import (
    CEF_DOWNLOAD_BASE,
    CEF_VERSION_FILE,
    compute_hash,
    download_file,
    fetch_index,
    find_version_by_prefix,
    get_minimal_distribution,
    read_pinned_version,
)

log = logging.getLogger(__name__)
MANIFEST_PATH = pathlib.Path(__file__).parent / "org.jellyfin.JellyfinDesktop.yml"


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    version = read_pinned_version()
    if not version:
        raise RuntimeError(f"CEF_VERSION file not found or empty: {CEF_VERSION_FILE}")

    if not MANIFEST_PATH.exists():
        raise RuntimeError(f"Flatpak manifest not found: {MANIFEST_PATH}")

    # Early exit if manifest already matches
    manifest_text = MANIFEST_PATH.read_text()
    if f"cef_binary_{version}" in manifest_text:
        log.info("Flatpak manifest already matches CEF_VERSION (%s)", version)
        return

    # Resolve version info for linux64
    index = fetch_index()
    version_data = find_version_by_prefix(index, "linux64", version)
    if not version_data:
        raise RuntimeError(f"Version {version} not found for linux64")

    file_info = get_minimal_distribution(version_data)
    filename = file_info["name"]
    url = f"{CEF_DOWNLOAD_BASE}/{filename}"
    expected_sha1 = file_info.get("sha1")

    # Download tarball to compute sha256
    with tempfile.TemporaryDirectory() as tmpdir:
        tarball = pathlib.Path(tmpdir) / filename
        log.info("Downloading %s to compute sha256...", filename)
        download_file(url, tarball, expected_sha1)
        sha256 = compute_hash(tarball, "sha256")

    log.info("Updating Flatpak manifest")
    log.info("  URL: %s", url)
    log.info("  SHA256: %s", sha256)

    # Patch manifest in-place
    lines = manifest_text.splitlines()
    result = []
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.lstrip()

        if stripped.startswith("url: https://cef-builds.spotifycdn.com/cef_binary_"):
            indent = line[: len(line) - len(stripped)]
            result.append(f"{indent}url: {url}")
            # Replace next hash line
            i += 1
            if i < len(lines) and lines[i].lstrip().startswith(("sha256:", "sha1:", "sha512:")):
                result.append(f"{indent}sha256: {sha256}")
                i += 1
            continue

        result.append(line)
        i += 1

    MANIFEST_PATH.write_text("\n".join(result) + "\n")
    log.info("Done")


if __name__ == "__main__":
    main()
