"""Update CEF: write CEF_VERSION and sync Flatpak manifest."""

import logging
import pathlib
import subprocess
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR.parent))

from download_cef import (
    CEF_VERSION_FILE,
    read_pinned_version,
    relpath,
    resolve_distribution,
)

log = logging.getLogger(__name__)

FLATPAK_SCRIPT = TOOLS_DIR / "update_flatpak_manifest.py"


def check() -> bool:
    """Return True if CEF_VERSION matches latest stable and Flatpak manifest is in sync."""
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


def update() -> None:
    """Write CEF_VERSION to latest stable and sync the Flatpak manifest."""
    dist = resolve_distribution(platform_id="linux64")
    version = dist["cef_version"]
    log.info("Latest stable CEF: %s", version)

    if read_pinned_version() == version:
        log.info("%s already at %s", relpath(CEF_VERSION_FILE), version)
    else:
        CEF_VERSION_FILE.write_text(version + "\n")
        log.info("Wrote %s -> %s", relpath(CEF_VERSION_FILE), version)

    subprocess.run([sys.executable, str(FLATPAK_SCRIPT)], check=True)
