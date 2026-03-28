#!/usr/bin/env -S uv run --script
# /// script
# dependencies = [
#   "psutil",
# ]
# ///
import sys
import time

import psutil


def find_procs(name):
    procs = []
    for p in psutil.process_iter(["name"]):
        if p.info["name"] == name:
            procs.append(p)
    return procs


def get_ctx_switches(procs):
    total = 0
    for p in procs:
        try:
            cs = p.num_ctx_switches()
            total += cs.voluntary + cs.involuntary
        except psutil.NoSuchProcess:
            pass
    return total


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "jellyfin-desktop"
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5

    # Prime cpu_percent
    procs = find_procs(name)
    for p in procs:
        try:
            p.cpu_percent()
        except psutil.NoSuchProcess:
            pass
    prev_cs = get_ctx_switches(procs)

    time.sleep(interval)

    samples = []
    try:
        while True:
            procs = find_procs(name)
            total = 0.0
            for p in procs:
                try:
                    total += p.cpu_percent()
                except psutil.NoSuchProcess:
                    pass

            curr_cs = get_ctx_switches(procs)
            wakes = int((curr_cs - prev_cs) / interval)
            prev_cs = curr_cs

            if procs:
                samples.append((total, wakes))
                print(f"{total:.1f}%  {wakes} wakes/s  ({len(procs)} pids)")
            else:
                print(f"{name}: not running")
            time.sleep(interval)
    except KeyboardInterrupt:
        pass

    if samples:
        avg_cpu = sum(s[0] for s in samples) / len(samples)
        avg_wakes = sum(s[1] for s in samples) / len(samples)
        print(f"\navg: {avg_cpu:.1f}%  {avg_wakes:.0f} wakes/s  ({len(samples)} samples)")


if __name__ == "__main__":
    main()
