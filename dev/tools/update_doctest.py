"""Update vendored doctest single-header to latest upstream release."""

import json
import logging
import pathlib
import urllib.request

log = logging.getLogger(__name__)

REPO = "doctest/doctest"
VENDOR_DIR = pathlib.Path(__file__).resolve().parent.parent.parent / "third_party" / "doctest"
VERSION_FILE = VENDOR_DIR / "VERSION"
HEADER_FILE = VENDOR_DIR / "doctest.h"


def latest_tag() -> str:
    url = f"https://api.github.com/repos/{REPO}/releases/latest"
    with urllib.request.urlopen(url) as resp:
        data = json.load(resp)
    return data["tag_name"]


def read_pinned() -> str:
    return VERSION_FILE.read_text().strip()


def check() -> bool:
    current = read_pinned()
    latest = latest_tag()
    if current != latest:
        log.info("doctest out of date: %s (latest: %s)", current, latest)
        return False
    log.info("doctest at %s", current)
    return True


def update() -> None:
    latest = latest_tag()
    log.info("Latest doctest: %s", latest)

    if read_pinned() == latest:
        log.info("doctest already at %s", latest)
        return

    header_url = f"https://raw.githubusercontent.com/{REPO}/{latest}/doctest/doctest.h"
    log.info("Downloading %s", header_url)
    with urllib.request.urlopen(header_url) as resp:
        HEADER_FILE.write_bytes(resp.read())

    VERSION_FILE.write_text(latest + "\n")
    log.info("Updated doctest to %s", latest)
