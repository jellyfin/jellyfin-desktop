#!/usr/bin/env python3
"""Inject a <release> entry into the AppStream metainfo template."""
import argparse
import pathlib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--date", required=True)
    args = ap.parse_args()

    lines = args.template.read_text().splitlines(keepends=True)
    out = []
    injected = False
    for line in lines:
        out.append(line)
        if not injected and "<releases>" in line:
            indent = line[: len(line) - len(line.lstrip())]
            out.append(
                f'{indent}  <release version="{args.version}" date="{args.date}"/>\n'
            )
            injected = True
    if not injected:
        raise SystemExit("error: <releases> tag not found in template")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("".join(out))


if __name__ == "__main__":
    main()
