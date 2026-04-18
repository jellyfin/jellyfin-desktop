#!/usr/bin/env python3
"""Update all vendored/fetched dependencies, or check for staleness."""

import argparse
import logging
import sys

import update_cef
import update_doctest
import update_quill


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if any dep is stale",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")

    if args.check:
        results = [update_cef.check(), update_doctest.check(), update_quill.check()]
        sys.exit(0 if all(results) else 1)

    update_cef.update()
    update_doctest.update()
    update_quill.update()


if __name__ == "__main__":
    main()
