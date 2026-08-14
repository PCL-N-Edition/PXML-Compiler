#!/usr/bin/env python3
"""Convert verified release binary-diff records into a stable JSON manifest."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--records", type=pathlib.Path, required=True)
    parser.add_argument("--previous-tag", required=True)
    parser.add_argument("--target-tag", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    entries = []
    with args.records.open("r", encoding="utf-8", newline="") as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if len(row) != 9:
                raise SystemExit(f"invalid diff record: {row!r}")
            tool, target, patch, source_hash, target_hash, patch_hash, source_size, target_size, patch_size = row
            entries.append(
                {
                    "tool": tool,
                    "target": target,
                    "patch": patch,
                    "sourceSha256": source_hash,
                    "targetSha256": target_hash,
                    "patchSha256": patch_hash,
                    "sourceSize": int(source_size),
                    "targetSize": int(target_size),
                    "patchSize": int(patch_size),
                }
            )
    manifest = {
        "schemaVersion": 1,
        "algorithm": "bsdiff40",
        "sourceTag": args.previous_tag,
        "targetTag": args.target_tag,
        "entries": sorted(entries, key=lambda entry: (entry["target"], entry["tool"])),
    }
    args.output.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
