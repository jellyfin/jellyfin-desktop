"""Update vendored quill source tree to latest upstream release."""

import io
import json
import logging
import pathlib
import shutil
import tarfile
import tempfile
import urllib.request

log = logging.getLogger(__name__)

REPO = "odygrd/quill"
VENDOR_DIR = pathlib.Path(__file__).resolve().parent.parent.parent / "third_party" / "quill"
VERSION_FILE = VENDOR_DIR / "VERSION"


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
        log.info("quill out of date: %s (latest: %s)", current, latest)
        return False
    log.info("quill at %s", current)
    return True


def update() -> None:
    latest = latest_tag()
    log.info("Latest quill: %s", latest)

    if read_pinned() == latest:
        log.info("quill already at %s", latest)
        return

    tarball_url = f"https://github.com/{REPO}/archive/refs/tags/{latest}.tar.gz"
    log.info("Downloading %s", tarball_url)
    with urllib.request.urlopen(tarball_url) as resp:
        data = resp.read()

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            tar.extractall(tmp_path, filter="data")
        (extracted,) = tmp_path.iterdir()

        shutil.rmtree(VENDOR_DIR)
        shutil.move(str(extracted), str(VENDOR_DIR))

    log.info("Updated quill to %s", latest)
