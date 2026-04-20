"""Update CEF: write CEF_VERSION."""

import logging

from download_cef import (
    CEF_VERSION_FILE,
    read_pinned_version,
    relpath,
    resolve_distribution,
)

log = logging.getLogger(__name__)


def check() -> bool:
    """Return True if CEF_VERSION matches latest stable."""
    current = read_pinned_version()
    latest = resolve_distribution(platform_id="linux64")["cef_version"]
    if current != latest:
        log.info("CEF_VERSION out of date: %s (latest: %s)", current, latest)
        return False
    return True


def update() -> None:
    """Write CEF_VERSION to latest stable."""
    dist = resolve_distribution(platform_id="linux64")
    version = dist["cef_version"]
    log.info("Latest stable CEF: %s", version)

    if read_pinned_version() == version:
        log.info("%s already at %s", relpath(CEF_VERSION_FILE), version)
    else:
        CEF_VERSION_FILE.write_text(version + "\n")
        log.info("Wrote %s -> %s", relpath(CEF_VERSION_FILE), version)
