#!/usr/bin/env python3
#
#  Summarise an RT_STAGE_PROFILE JSONL into one row per node.
#
#  Usage: summarize-stage-profile.py <stages.jsonl>
#  Prints: node count total_ms max_ms impls cache_hits
#
import argparse
import collections
import json
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    args = parser.parse_args()

    rows = collections.OrderedDict()
    with open(args.profile, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                print(f"skipping malformed record: {line!r}", file=sys.stderr)
                continue
            stage = record.get("stage", "")
            if not stage:
                continue
            row = rows.setdefault(
                stage,
                {"count": 0, "ns": 0, "max_ns": 0, "impls": set(), "hits": 0},
            )
            row["count"] += 1
            elapsed = int(record.get("ns", 0))
            row["ns"] += elapsed
            row["max_ns"] = max(row["max_ns"], elapsed)
            row["impls"].add(record.get("impl", "?"))
            row["hits"] += int(record.get("cache_hit", 0))

    print("node\tcount\ttotal_ms\tmax_ms\timpls\tcache_hits")
    for stage, row in rows.items():
        impls = ",".join(sorted(row["impls"]))
        print(
            f"{stage}\t{row['count']}\t{row['ns'] / 1.0e6:.3f}\t"
            f"{row['max_ns'] / 1.0e6:.3f}\t{impls}\t{row['hits']}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
