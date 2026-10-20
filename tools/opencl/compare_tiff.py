#!/usr/bin/env python3
#
#  Compare two rendered TIFFs without loading both into ImageMagick at once.
#  Converts each to 16-bit RGB and compares in chunks.
#
#  Usage: compare_tiff.py A.tif B.tif [--bits N]
#  Prints: max_abs normalized_max differing total
#
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np


def convert(path, destination):
    subprocess.run(
        ["magick", path, "-depth", "16", "-endian", "LSB",
         "rgb:" + destination],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--bits", type=int, default=16)
    args = parser.parse_args()

    directory = tempfile.mkdtemp(prefix="rtcmp.", dir=os.environ.get("TMPDIR"))
    raw_a = os.path.join(directory, "a.raw")
    raw_b = os.path.join(directory, "b.raw")

    try:
        convert(args.a, raw_a)
        convert(args.b, raw_b)

        a = np.memmap(raw_a, dtype="<u2", mode="r")
        b = np.memmap(raw_b, dtype="<u2", mode="r")

        if len(a) != len(b):
            print("dimension mismatch", file=sys.stderr)
            return 2

        # One 8-bit step becomes 257 in the 16-bit scale.
        limit = 65535 // ((1 << args.bits) - 1)
        maximum = 0
        differing = 0
        chunk = 10_000_000

        for start in range(0, len(a), chunk):
            end = min(start + chunk, len(a))
            difference = np.abs(
                a[start:end].astype(np.int32) - b[start:end].astype(np.int32))
            maximum = max(maximum, int(difference.max()))
            differing += int((difference > 0).sum())

        normalized = maximum / 65535.0
        print(f"{maximum} {normalized:.8g} {differing} {len(a)}")
        return 0 if maximum <= limit else 1
    finally:
        for path in (raw_a, raw_b):
            try:
                os.remove(path)
            except FileNotFoundError:
                pass
        try:
            os.rmdir(directory)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
