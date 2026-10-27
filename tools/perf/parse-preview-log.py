#!/usr/bin/env python3
"""Parse RT_PREVIEW_PROFILE output into CSV and percentile summaries."""

import argparse
import json
import math
import re
import sys

LINE = re.compile(
    r"preview update:\s+(?P<ms>\d+)\s+ms\s+kind=(?P<kind>\d+)"
    r"\s+request=(?P<request>[0-9a-fA-F]{1,16})"
    r"(?:\s+cache=(?P<cache>\d+))?"
    r"(?:\s+viewport=(?P<viewport>[A-Za-z-]+))?"
    r"(?:\s+crop_ms=(?P<crop_ms>\d+))?"
    r"(?:\s+deferred_stages=0x(?P<deferred_stages>[0-9a-fA-F]+))?"
    r"(?:\s+deferred_hits=(?P<deferred_hits>\d+))?"
    r"(?:\s+reason=(?P<reason>[A-Za-z0-9_-]+))?"
)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1))
    return ordered[index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path", nargs="?", default="-")
    parser.add_argument("--csv")
    parser.add_argument("--json")
    parser.add_argument("--drop-first", action="store_true")
    args = parser.parse_args()

    stream = sys.stdin if args.path == "-" else open(args.path, encoding="utf-8")
    rows = []
    for line in stream:
        match = LINE.search(line)
        if not match:
            continue
        row = match.groupdict()
        row["ms"] = int(row["ms"])
        row["kind"] = int(row["kind"])
        row["request"] = int(row["request"], 16)
        if row["cache"] is not None:
            row["cache"] = int(row["cache"])
        if row["crop_ms"] is not None:
            row["crop_ms"] = int(row["crop_ms"])
        if row["deferred_stages"] is not None:
            row["deferred_stages"] = int(row["deferred_stages"], 16)
        if row["deferred_hits"] is not None:
            row["deferred_hits"] = int(row["deferred_hits"])
        rows.append(row)

    matched = len(rows)
    if args.drop_first and rows:
        rows = rows[1:]

    if not rows:
        if matched == 0:
            message = "no 'preview update:' samples found"
        else:
            message = "every 'preview update:' sample was dropped"
        print(f"parse-preview-log: {message} in {args.path}",
              file=sys.stderr)
        return 1

    if args.csv:
        fields = ["ms", "kind", "request", "cache", "viewport", "crop_ms",
                  "deferred_stages", "deferred_hits", "reason"]
        with open(args.csv, "w", encoding="utf-8") as handle:
            handle.write(",".join(fields) + "\n")
            for row in rows:
                handle.write(",".join("" if row[f] is None else str(row[f])
                                      for f in fields) + "\n")

    values = [row["ms"] for row in rows]
    summary = {
        "samples": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values) if values else None,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump({"summary": summary, "rows": rows}, handle, indent=2)
    else:
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
