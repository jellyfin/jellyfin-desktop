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


def fetch_index():
    """Fetch CEF builds index."""
    log.info("Fetching CEF builds index")
    with urllib.request.urlopen(CEF_INDEX_URL) as resp:
        return json.load(resp)


def find_version_by_prefix(index, platform_id, version_prefix):
    """Find a CEF version in the index matching the given prefix."""
    for v in index.get(platform_id, {}).get("versions", []):
        if v.get("cef_version", "").startswith(version_prefix):
            return v
    return None


def find_latest_stable(index, platform_id):
    """Find the latest stable CEF version for the given platform."""
    stable_versions = [
        v for v in index.get(platform_id, {}).get("versions", [])
        if v.get("channel") == "stable"
    ]
    if not stable_versions:
        raise RuntimeError(f"No stable version found for platform: {platform_id}")
    # Sort by chromium major version descending
    stable_versions.sort(
        key=lambda v: int(v.get("chromium_version", "0").split(".")[0]),
        reverse=True,
    )
    return stable_versions[0]


def get_minimal_distribution(version_data):
    """Get the minimal distribution file info."""
    for file_info in version_data.get("files", []):
        if file_info.get("type") == "minimal":
            return file_info
    # Fall back to standard if no minimal
    for file_info in version_data.get("files", []):
        if file_info.get("type") == "standard":
            return file_info
    raise RuntimeError("No suitable distribution found")


def compute_hash(path, algorithm="sha1"):
    """Compute hash of a file."""
    h = hashlib.new(algorithm)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_sha1(path, expected_sha1):
    """Verify SHA1 hash of a file."""
    log.info("Verifying SHA1")
    actual_sha1 = compute_hash(path, "sha1")
    if actual_sha1 != expected_sha1:
        raise RuntimeError(
            f"SHA1 mismatch: expected {expected_sha1}, got {actual_sha1}"
        )


def download_file(url, dest_path, expected_sha1=None):
    """Download a file with progress indication and optional hash verification."""
    log.info("Downloading %s", url)
    temp_path = dest_path.parent / f".{dest_path.name}.tmp"
    sha1_path = dest_path.parent / f"{dest_path.name}.sha1"

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

        if expected_sha1:
            verify_sha1(temp_path, expected_sha1)
            sha1_path.write_text(expected_sha1)

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

    # Clean up any previous temp dir
    if temp_dir.exists():
        shutil.rmtree(temp_dir)
    temp_dir.mkdir(parents=True)

    try:
        with tarfile.open(tarball_path, "r:bz2") as tar:
            tar.extractall(temp_dir)
        # Move extracted content (it's in a subdir) to final location
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
    if link_path.is_symlink() or (hasattr(link_path, 'is_junction') and link_path.is_junction()):
        link_path.unlink()
    elif link_path.exists():
        shutil.rmtree(link_path)
    if platform.system() == "Windows":
        # Use directory junction (no special permissions needed)
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
        choices=["linux64", "linuxarm64", "macosx64", "macosarm64", "windows64", "windowsarm64"],
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

    # Check for existing tarball and sha1
    existing = find_existing_tarball(args.output_dir, platform_id)
    if existing and args.version:
        _, existing_name = existing
        if not existing_name.startswith(f"cef_binary_{args.version}"):
            log.info("Existing CEF doesn't match requested version %s, re-downloading",
                     args.version)
            existing = None
    if existing:
        tarball_path, versioned_dir_name = existing
        sha1_path = tarball_path.parent / f"{tarball_path.name}.sha1"
        have_sha1 = sha1_path.exists()
    else:
        have_sha1 = False

    # Fetch index if needed (no tarball, no sha1, or --show-latest)
    if args.show_latest or not existing or not have_sha1:
        index = fetch_index()

        if args.version:
            version_data = find_version_by_prefix(index, platform_id, args.version)
            if not version_data:
                raise RuntimeError(
                    f"Version {args.version} not found for {platform_id}"
                )
        else:
            version_data = find_latest_stable(index, platform_id)

        cef_version = version_data.get("cef_version", "unknown")
        chromium_version = version_data.get("chromium_version", "unknown")
        file_info = get_minimal_distribution(version_data)
        filename = file_info["name"]
        expected_sha1 = file_info.get("sha1")
        download_url = f"{CEF_DOWNLOAD_BASE}/{filename}"

        if args.show_latest:
            print(
                json.dumps(
                    {
                        "platform": platform_id,
                        "cef_version": cef_version,
                        "chromium_version": chromium_version,
                        "url": download_url,
                        "filename": filename,
                        "sha1": expected_sha1,
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
        expected_sha1 = sha1_path.read_text().strip()

    log.info("Platform: %s", platform_id)
    log.info("Version: %s", versioned_dir_name)

    versioned_dir = args.output_dir / versioned_dir_name

    # Check if already at correct version
    is_link = cef_link.is_symlink() or (hasattr(cef_link, 'is_junction') and cef_link.is_junction())
    if is_link:
        current_target = os.readlink(cef_link)
        if current_target == versioned_dir_name and versioned_dir.exists():
            log.info("Skipping, already set up")
            return

    # Ensure output directory exists
    args.output_dir.mkdir(parents=True, exist_ok=True)

    # Download
    sha1_path = tarball_path.parent / f"{tarball_path.name}.sha1"
    if tarball_path.exists():
        log.info("Skipping download, already exists")
        # Verify existing tarball before extract
        if expected_sha1:
            verify_sha1(tarball_path, expected_sha1)
            if not sha1_path.exists():
                sha1_path.write_text(expected_sha1)
    else:
        download_file(download_url, tarball_path, expected_sha1)

    # Extract
    if versioned_dir.exists():
        log.info("Skipping extraction, already extracted")
    else:
        extract_tarball(tarball_path, args.output_dir)

    # Symlink
    log.info(
        "Creating symlink: %s -> %s", relpath(cef_link), versioned_dir_name
    )
    create_symlink(versioned_dir, cef_link)


if __name__ == "__main__":
    main()
