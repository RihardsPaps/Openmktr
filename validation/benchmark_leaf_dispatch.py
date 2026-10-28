#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise Kbuild-style shared dependency leaves without compiling releases.

One discarded warmup pair and seven alternating measured pairs per job count.
Synthetic evidence only; both binaries must use native Linux storage.
"""
import argparse
import json
import os
from pathlib import Path

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
    headers = [f"header-{i:04d}" for i in range(512)]
    targets = [f"target-{i:04d}" for i in range(128)]
    for name in headers:
        (root / name).write_bytes(b"input\n")
    (root / "Makefile").write_text(
        "INPUTS := " + " ".join(headers) + "\nTARGETS := " + " ".join(targets) +
        "\n.PHONY: all $(TARGETS)\nall: $(TARGETS)\n"
        "$(INPUTS):\n$(TARGETS): $(INPUTS)\n\t@:\n")
    rows = []
    for jobs in (8, 16):
        for sample in range(8):
            order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
            for variant in order:
                phase = run([str(tools[variant]), f"-j{jobs}", "all"], root,
                            root / f"j{jobs}-s{sample}-{variant}.log", env,
                            root / "commands.jsonl")
                if any((root / name).read_bytes() != b"input\n" for name in headers):
                    raise RuntimeError("Dependency leaf contents changed")
                rows.append(dict(project="synthetic", entrypoint="direct", jobs=jobs,
                                 scenario="shared_leaves", sample=sample, warmup=sample == 0,
                                 variant=variant, total_seconds=phase["seconds"], phase=phase))
    report = dict(protocol="one discarded warmup pair; seven alternating measured pairs",
                  scope="Synthetic dependency dispatch only; no release qualification",
                  binaries={v: digest(t) for v, t in tools.items()},
                  harness_sha256=digest(Path(__file__)), input_sha256=digest(root / "Makefile"),
                  samples=rows, summary=paired_summary(rows))
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
