#!/usr/bin/env python3
"""Download and extract CEF (Chromium Embedded Framework) distribution."""

import argparse
import glob
import hashlib
import json
import logging
import os
import pathlib
import platform
import shutil
import sys
import tarfile
import urllib.request

CEF_INDEX_URL = "https://cef-builds.spotifycdn.com/index.json"
CEF_DOWNLOAD_BASE = "https://cef-builds.spotifycdn.com"

PLATFORM_MAP = {
    ("Linux", "x86_64"): "linux64",
    ("Linux", "aarch64"): "linuxarm64",
    ("Darwin", "x86_64"): "macosx64",
    ("Darwin", "arm64"): "macosarm64",
    ("Windows", "AMD64"): "windows64",
    ("Windows", "ARM64"): "windowsarm64",
}

log = logging.getLogger(__name__)
REPO_ROOT = pathlib.Path(__file__).parent.parent
CEF_VERSION_FILE = REPO_ROOT / "CEF_VERSION"


def relpath(path):
    """Return path relative to repo root."""
    try:
        return pathlib.Path(path).relative_to(REPO_ROOT)
    except ValueError:
        return path


def get_platform_id():
    """Detect current platform and return CEF platform identifier."""
    system = platform.system()
    machine = platform.machine()
    key = (system, machine)
    if key not in PLATFORM_MAP:
        raise RuntimeError(f"Unsupported platform: {system} {machine}")
    return PLATFORM_MAP[key]


def _fetch_index():
    log.info("Fetching CEF builds index")
    with urllib.request.urlopen(CEF_INDEX_URL) as resp:
        return json.load(resp)


def _find_version_by_prefix(index, platform_id, version_prefix):
    for v in index.get(platform_id, {}).get("versions", []):
        if v.get("cef_version", "").startswith(version_prefix):
            return v
    return None


def _find_latest_stable(index, platform_id):
    stable_versions = [
        v
        for v in index.get(platform_id, {}).get("versions", [])
        if v.get("channel") == "stable"
    ]
    if not stable_versions:
        raise RuntimeError(
            f"No stable version found for platform: {platform_id}"
        )
    stable_versions.sort(
        key=lambda v: int(v.get("chromium_version", "0").split(".")[0]),
        reverse=True,
    )
    return stable_versions[0]


def _get_minimal_distribution(version_data):
    for file_info in version_data.get("files", []):
        if file_info.get("type") == "minimal":
            return file_info
    for file_info in version_data.get("files", []):
        if file_info.get("type") == "standard":
            return file_info
    raise RuntimeError("No suitable distribution found")


def _fetch_sha256(tarball_url):
    sha256_url = f"{tarball_url}.sha256"
    log.info("Fetching %s", sha256_url)
    with urllib.request.urlopen(sha256_url) as resp:
        sha256 = resp.read().decode().strip()
    if len(sha256) != 64 or not all(c in "0123456789abcdef" for c in sha256):
        raise RuntimeError(f"Invalid sha256 from {sha256_url}: {sha256!r}")
    return sha256


def resolve_distribution(version=None, platform_id=None):
    """Resolve a CEF version (or latest stable if None) to distribution info.

    Returns a dict with: cef_version, chromium_version, filename, url, sha256.
    """
    platform_id = platform_id or get_platform_id()
    index = _fetch_index()
    if version:
        data = _find_version_by_prefix(index, platform_id, version)
        if not data:
            raise RuntimeError(
                f"Version {version} not found for {platform_id}"
            )
    else:
        data = _find_latest_stable(index, platform_id)
    file_info = _get_minimal_distribution(data)
    filename = file_info["name"]
    url = f"{CEF_DOWNLOAD_BASE}/{filename}"
    return {
        "cef_version": data.get("cef_version"),
        "chromium_version": data.get("chromium_version"),
        "filename": filename,
        "url": url,
        "sha256": _fetch_sha256(url),
    }


def compute_hash(path, algorithm="sha256"):
    """Compute hash of a file."""
    h = hashlib.new(algorithm)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_sha256(path, expected_sha256):
    """Verify SHA256 hash of a file."""
    log.info("Verifying SHA256")
    actual = compute_hash(path, "sha256")
    if actual != expected_sha256:
        raise RuntimeError(
            f"SHA256 mismatch: expected {expected_sha256}, got {actual}"
        )


def download_file(url, dest_path, expected_sha256=None):
    """Download a file with progress indication and optional hash verification."""
    log.info("Downloading %s", url)
    temp_path = dest_path.parent / f".{dest_path.name}.tmp"
    sha256_path = dest_path.parent / f"{dest_path.name}.sha256"

    def report_progress(block_num, block_size, total_size):
        downloaded = block_num * block_size
        if total_size > 0:
            percent = min(100, downloaded * 100 // total_size)
            mb_downloaded = downloaded / (1024 * 1024)
            mb_total = total_size / (1024 * 1024)
            sys.stdout.write(
                f"\r  {mb_downloaded:.1f}/{mb_total:.1f} MB ({percent}%)"
            )
            sys.stdout.flush()

    try:
        urllib.request.urlretrieve(url, temp_path, reporthook=report_progress)
        print()  # newline after progress

        if expected_sha256:
            verify_sha256(temp_path, expected_sha256)
            sha256_path.write_text(expected_sha256)

        temp_path.rename(dest_path)
    except:
        temp_path.unlink(missing_ok=True)
        raise


def extract_tarball(tarball_path, extract_dir):
    """Extract tarball to directory, returns extracted dir name."""
    root_dir = tarball_path.name.removesuffix(".tar.bz2")
    final_dir = extract_dir / root_dir
    temp_dir = extract_dir / ".cef_extract_tmp"

    log.info("Extracting to %s", relpath(final_dir))

    if temp_dir.exists():
        shutil.rmtree(temp_dir)
    temp_dir.mkdir(parents=True)

    try:
        with tarfile.open(tarball_path, "r:bz2") as tar:
            tar.extractall(temp_dir)
        extracted = temp_dir / root_dir
        extracted.rename(final_dir)
        temp_dir.rmdir()
    except:
        if temp_dir.exists():
            shutil.rmtree(temp_dir)
        raise

    return root_dir


def create_symlink(target_dir, link_path):
    """Create or update symlink (uses directory junction on Windows)."""
    if link_path.is_symlink() or (
        hasattr(link_path, "is_junction") and link_path.is_junction()
    ):
        link_path.unlink()
    elif link_path.exists():
        shutil.rmtree(link_path)
    if platform.system() == "Windows":
        import subprocess

        target_abs = (link_path.parent / target_dir.name).resolve()
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link_path), str(target_abs)],
            check=True,
        )
    else:
        link_path.symlink_to(target_dir.name)


def find_existing_tarball(output_dir, platform_id):
    """Find existing CEF tarball for platform."""
    pattern = str(output_dir / f"cef_binary_*_{platform_id}_minimal.tar.bz2")
    tarballs = sorted(glob.glob(pattern), reverse=True)  # newest first by name
    if tarballs:
        tarball_path = pathlib.Path(tarballs[0])
        versioned_dir_name = tarball_path.name.removesuffix(".tar.bz2")
        return tarball_path, versioned_dir_name
    return None


def read_pinned_version():
    """Read pinned CEF version from CEF_VERSION file, if it exists."""
    if CEF_VERSION_FILE.exists():
        version = CEF_VERSION_FILE.read_text().strip()
        if version:
            return version
    return None


def main():
    pinned = read_pinned_version()

    parser = argparse.ArgumentParser(description="Download CEF distribution")
    parser.add_argument(
        "--platform",
        choices=[
            "linux64",
            "linuxarm64",
            "macosx64",
            "macosarm64",
            "windows64",
            "windowsarm64",
        ],
        help="Target platform (default: auto-detect)",
    )
    parser.add_argument(
        "--version",
        default=pinned,
        help="CEF version to download (default: from CEF_VERSION file, or latest stable)",
    )
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).parent.parent / "third_party",
        help="Output directory (default: third_party/)",
    )
    parser.add_argument(
        "--show-latest",
        action="store_true",
        help="Output JSON with version info, don't download",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    platform_id = args.platform or get_platform_id()
    cef_link = args.output_dir / "cef"

    existing = find_existing_tarball(args.output_dir, platform_id)
    if existing and args.version:
        _, existing_name = existing
        if not existing_name.startswith(f"cef_binary_{args.version}"):
            log.info(
                "Existing CEF doesn't match requested version %s, re-downloading",
                args.version,
            )
            existing = None
    if existing:
        tarball_path, versioned_dir_name = existing
        sha256_path = tarball_path.parent / f"{tarball_path.name}.sha256"
        have_sha256 = sha256_path.exists()
    else:
        have_sha256 = False

    if args.show_latest or not existing or not have_sha256:
        dist = resolve_distribution(args.version or None, platform_id)
        filename = dist["filename"]
        download_url = dist["url"]
        expected_sha256 = dist["sha256"]

        if args.show_latest:
            print(
                json.dumps(
                    {
                        "platform": platform_id,
                        "cef_version": dist["cef_version"],
                        "chromium_version": dist["chromium_version"],
                        "url": download_url,
                        "filename": filename,
                        "sha256": expected_sha256,
                        "tarball_path": str(args.output_dir / filename),
                        "extract_path": str(args.output_dir / "cef"),
                    },
                    indent=2,
                )
            )
            return

        tarball_path = args.output_dir / filename
        versioned_dir_name = filename.removesuffix(".tar.bz2")
    else:
        expected_sha256 = sha256_path.read_text().strip()

    log.info("Platform: %s", platform_id)
    log.info("Version: %s", versioned_dir_name)

    versioned_dir = args.output_dir / versioned_dir_name

    is_link = cef_link.is_symlink() or (
        hasattr(cef_link, "is_junction") and cef_link.is_junction()
    )
    if is_link:
        current_target = os.readlink(cef_link)
        if current_target == versioned_dir_name and versioned_dir.exists():
            log.info("Skipping, already set up")
            return

    args.output_dir.mkdir(parents=True, exist_ok=True)

    sha256_path = tarball_path.parent / f"{tarball_path.name}.sha256"
    if tarball_path.exists():
        log.info("Skipping download, already exists")
        if expected_sha256:
            verify_sha256(tarball_path, expected_sha256)
            if not sha256_path.exists():
                sha256_path.write_text(expected_sha256)
    else:
        download_file(download_url, tarball_path, expected_sha256)

    if versioned_dir.exists():
        log.info("Skipping extraction, already extracted")
    else:
        extract_tarball(tarball_path, args.output_dir)

    log.info(
        "Creating symlink: %s -> %s", relpath(cef_link), versioned_dir_name
    )
    create_symlink(versioned_dir, cef_link)


if __name__ == "__main__":
    main()
