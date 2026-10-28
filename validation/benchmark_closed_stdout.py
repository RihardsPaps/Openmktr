#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Isolate recipe-supervisor exit latency using a small synthetic Makefile.

Both binaries must be on the same native Linux filesystem. This does not
qualify release workloads or consume their large-build allowance.
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
    (root / "Makefile").write_text(
        "".join(f"V{i} := $(shell printf x; exec 1>&-; sleep .001)\n" for i in range(128)) +
        "all:\n\t@printf '%s' '" + "".join(f"$(V{i})" for i in range(128)) + "' > result\n")
    rows = []
    for sample in range(8):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            phase = run([str(tools[variant]), "-j1", "all"], root,
                        root / f"s{sample}-{variant}.log", env, root / "commands.jsonl")
            if (root / "result").read_bytes() != b"x" * 128:
                raise RuntimeError("Synthetic shell probes returned incorrect output")
            rows.append(dict(project="synthetic", entrypoint="direct", jobs=1,
                             scenario="closed_stdout", sample=sample, warmup=sample == 0,
                             variant=variant, total_seconds=phase["seconds"], phase=phase))
    report = dict(protocol="one discarded warmup pair; seven alternating measured pairs",
                  scope="Synthetic exit-latency mechanism only; no release qualification",
                  binaries={v: digest(t) for v, t in tools.items()},
                  harness_sha256=digest(Path(__file__)),
                  input_sha256=digest(root / "Makefile"), samples=rows,
                  summary=paired_summary(rows))
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
