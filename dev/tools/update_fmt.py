"""Update vendored fmt source tree to latest upstream release."""

import io
import json
import logging
import pathlib
import shutil
import tarfile
import urllib.request

log = logging.getLogger(__name__)

REPO = "fmtlib/fmt"
VENDOR_DIR = pathlib.Path(__file__).resolve().parent.parent.parent / "third_party" / "fmt"
VERSION_FILE = VENDOR_DIR / "VERSION"
KEEP_TOP = {"CMakeLists.txt", "LICENSE", "README.md", "ChangeLog.md"}
KEEP_DIRS = {"include", "src", "support"}


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
        log.info("fmt out of date: %s (latest: %s)", current, latest)
        return False
    log.info("fmt at %s", current)
    return True


def update() -> None:
    latest = latest_tag()
    log.info("Latest fmt: %s", latest)

    if read_pinned() == latest:
        log.info("fmt already at %s", latest)
        return

    url = f"https://github.com/{REPO}/archive/refs/tags/{latest}.tar.gz"
    log.info("Downloading %s", url)
    with urllib.request.urlopen(url) as resp:
        blob = resp.read()

    # Clear the vendor dir except VERSION (we'll rewrite at the end).
    for child in VENDOR_DIR.iterdir():
        if child.name == "VERSION":
            continue
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()

    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tar:
        root = tar.getnames()[0].split("/")[0]
        for member in tar.getmembers():
            parts = member.name.split("/", 1)
            if len(parts) < 2:
                continue
            rel = parts[1]
            if not rel:
                continue
            top = rel.split("/", 1)[0]
            if top not in KEEP_TOP and top not in KEEP_DIRS:
                continue
            member.name = rel
            tar.extract(member, VENDOR_DIR)
        _ = root

    VERSION_FILE.write_text(latest + "\n")
    log.info("Updated fmt to %s", latest)
