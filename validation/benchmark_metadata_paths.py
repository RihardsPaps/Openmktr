#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Measure cached timestamp queries with owned long path strings.

Synthetic diagnostic only; does not qualify release workloads or consume
their large-build allowance. Both binaries must use native Linux storage.
"""
import argparse
import json
import os
from pathlib import Path
import re
import time

from benchmark_ninja import digest, paired_summary, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    tools = dict(baseline=args.baseline.resolve(), candidate=args.candidate.resolve())
    env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_") and
           k not in ("MAKEFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "MFLAGS")}
    env.update(CCACHE_DISABLE="1", LC_ALL="C", TZ="UTC")
    old = time.time_ns() - 30_000_000_000
    names = ["input-" + "x" * 64 + f"-{i:04d}" for i in range(512)]
    for name in names:
        path = root / name
        path.write_bytes(b"input\n")
        os.utime(path, ns=(old, old))
    output = root / "cached"
    output.write_bytes(b"settled\n")
    stamp = old + 10_000_000_000
    os.utime(output, ns=(stamp, stamp))
    (root / "Makefile").write_text(
        ".NOTPARALLEL:\n.PHONY: all FORCE\nall: cached\nREPEAT := " + "x " * 2048 +
        "\ncached: " + " ".join(names) + " FORCE\n"
        "\t@$(foreach x,$(REPEAT),$(if $? ,:,:))\n")
    rows = []
    for sample in range(8):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            log = root / f"s{sample}-{variant}.log"
            phase = run([str(tools[variant]), "-j1", "--kati_stats", "all"], root,
                        log, env, root / "commands.jsonl")
            hits = sum(map(int, re.findall(r"recipe metadata probes: (\d+) hits", log.read_text())))
            if hits < 1_000_000:
                raise RuntimeError("Diagnostic did not exercise the cached timestamp path")
            if output.read_bytes() != b"settled\n" or output.stat().st_mtime_ns != stamp:
                raise RuntimeError("Pure timestamp probes modified their output")
            rows.append(dict(project="synthetic", entrypoint="direct", jobs=1,
                             scenario="metadata_paths", sample=sample, warmup=sample == 0,
                             variant=variant, total_seconds=phase["seconds"],
                             metadata_hits=hits, phase=phase))
    report = dict(protocol="one discarded warmup pair; seven alternating measured pairs",
                  scope="Synthetic metadata allocation path only; no release qualification",
                  binaries={v: digest(t) for v, t in tools.items()},
                  harness_sha256=digest(Path(__file__)), input_sha256=digest(root / "Makefile"),
                  samples=rows, summary=paired_summary(rows))
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
