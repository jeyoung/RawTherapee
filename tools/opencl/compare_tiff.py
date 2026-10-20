#!/usr/bin/env python3
#
#  Compare two rendered TIFFs without loading both into memory at once.
#  ImageMagick normalises both inputs to uncompressed 16-bit TIFF, then the
#  encoded samples are compared in row chunks.
#
#  Usage: compare_tiff.py A.tif B.tif [--bits N]
#  Prints: max_abs normalized_max differing total
#
import argparse
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np


CHUNK_ROWS = 256


def normalise(path, destination):
    subprocess.run(
        ["magick", path, "-depth", "16", "-endian", "LSB",
         "-compress", "none", "tiff:" + destination],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class TiffReader:
    def __init__(self, path):
        self.file = open(path, "rb")
        header = self.file.read(8)

        if len(header) != 8 or header[:2] not in (b"II", b"MM"):
            raise ValueError("not a TIFF file")

        self.endian = "<" if header[:2] == b"II" else ">"
        ifd = struct.unpack(self.endian + "I", header[4:8])[0]

        self.file.seek(ifd)
        count = struct.unpack(self.endian + "H", self.file.read(2))[0]
        tags = {}

        for _ in range(count):
            entry = self.file.read(12)
            tag, kind, number = struct.unpack(self.endian + "HHI", entry[:8])
            tags[tag] = (kind, number, entry[8:12])

        self.width = self._scalar(tags, 256)
        self.height = self._scalar(tags, 257)
        self.bits = self._scalar(tags, 258, 16)
        self.compression = self._scalar(tags, 259, 1)
        self.samples = self._scalar(tags, 277, 3)
        self.rows_per_strip = self._scalar(tags, 278, self.height)
        self.planar = self._scalar(tags, 284, 1)
        self.offsets = self._array(tags, 273)
        self.counts = self._array(tags, 279)

        if self.compression != 1:
            raise ValueError("compressed TIFF is not supported")
        if self.planar != 1:
            raise ValueError("planar TIFF is not supported")
        if self.bits not in (8, 16):
            raise ValueError(f"unsupported bit depth: {self.bits}")
        if len(self.offsets) != len(self.counts):
            raise ValueError("strip table is inconsistent")

        self.dtype = np.dtype(self.endian + ("u1" if self.bits == 8 else "u2"))
        self.row_bytes = self.width * self.samples * self.dtype.itemsize

    def _scalar(self, tags, tag, default=None):
        if tag not in tags:
            if default is not None:
                return default
            raise ValueError(f"missing TIFF tag {tag}")

        kind, number, raw = tags[tag]

        if number > 1:
            return self._array(tags, tag)[0]
        if kind == 3:
            return struct.unpack(self.endian + "H", raw[:2])[0]
        if kind == 4:
            return struct.unpack(self.endian + "I", raw[:4])[0]
        raise ValueError(f"unsupported TIFF tag type {kind}")

    def _array(self, tags, tag):
        kind, number, raw = tags[tag]
        size = 2 if kind == 3 else 4
        fmt = "H" if kind == 3 else "I"

        if number == 1:
            value = struct.unpack(self.endian + fmt, raw[:size])[0]
            return [value]

        offset = struct.unpack(self.endian + "I", raw[:4])[0]
        position = self.file.tell()
        self.file.seek(offset)
        values = list(struct.unpack(
            self.endian + fmt * number, self.file.read(size * number)))
        self.file.seek(position)
        return values

    def rows(self, start, count):
        result = np.empty((count, self.width, self.samples), dtype=self.dtype)
        position = 0

        while position < count:
            row = start + position
            strip = row // self.rows_per_strip
            row_in_strip = row % self.rows_per_strip
            take = min(self.rows_per_strip - row_in_strip, count - position)

            if strip >= len(self.offsets):
                raise ValueError("strip index out of range")

            self.file.seek(self.offsets[strip] + row_in_strip * self.row_bytes)
            buffer = self.file.read(take * self.row_bytes)
            if len(buffer) != take * self.row_bytes:
                raise ValueError("short TIFF strip")

            result[position:position + take] = np.frombuffer(
                buffer, dtype=self.dtype).reshape(
                    take, self.width, self.samples)
            position += take

        return result.reshape(-1)

    def close(self):
        self.file.close()


def compare(a_path, b_path):
    a = TiffReader(a_path)
    b = TiffReader(b_path)

    try:
        if (a.width, a.height, a.samples) != (b.width, b.height, b.samples):
            print("dimension mismatch", file=sys.stderr)
            return 2

        maximum = 0
        differing = 0
        total = 0

        for y in range(0, a.height, CHUNK_ROWS):
            count = min(CHUNK_ROWS, a.height - y)
            left = a.rows(y, count).astype(np.int32)
            right = b.rows(y, count).astype(np.int32)

            if a.bits == 8:
                left *= 257
                right *= 257

            difference = np.abs(left - right)
            maximum = max(maximum, int(difference.max()))
            differing += int((difference > 0).sum())
            total += difference.size

        return maximum, differing, total
    finally:
        a.close()
        b.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--bits", type=int, default=16)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="rtcmp.") as directory:
        a = os.path.join(directory, "a.tif")
        b = os.path.join(directory, "b.tif")
        normalise(args.a, a)
        normalise(args.b, b)
        result = compare(a, b)

    if isinstance(result, int):
        return result

    maximum, differing, total = result
    # One 8-bit step becomes 257 in the 16-bit scale.
    limit = 65535 // ((1 << args.bits) - 1)
    normalized = maximum / 65535.0
    print(f"{maximum} {normalized:.8g} {differing} {total}")
    return 0 if maximum <= limit else 1


if __name__ == "__main__":
    sys.exit(main())
