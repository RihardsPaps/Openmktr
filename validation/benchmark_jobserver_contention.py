#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Measure recursive FIFO contention using only small synthetic Makefiles.

One discarded warmup pair and seven alternating measured pairs. This is
mechanism evidence, not Linux/FFmpeg qualification or a large-build reservation.
"""
import argparse
import json
import os
from pathlib import Path
import shutil

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
    rows = []
    for sample in range(8):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            build = root / f"s{sample}-{variant}"
            build.mkdir()
            branches = [f"child{i}" for i in range(16)]
            (build / "Makefile").write_text(
                ".PHONY: all " + " ".join(branches) + "\nall: " + " ".join(branches) + "\n" +
                "".join(f'{b}:\n\t@"$(MAKE)" -C {b} all\n' for b in branches))
            for branch in branches:
                child = build / branch
                child.mkdir()
                jobs = [f"job{i}" for i in range(32)]
                (child / "Makefile").write_text(
                    ".PHONY: all " + " ".join(jobs) + "\nall: " + " ".join(jobs) + "\n" +
                    "".join(f"job{i}:\n\t@printf built > result-{i}\n" for i in range(32)))
            source_hashes = {str(p.relative_to(build)): digest(p) for p in build.rglob("Makefile")}
            fifo = build / "fifo"
            os.mkfifo(fifo)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            try:
                os.write(fd, b"xx")
                env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_")
                       and k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
                env.update(KATI_JOBSERVER_FIFO=str(fifo), KATI_JOBSERVER_RESERVED="0",
                           CCACHE_DISABLE="1", LC_ALL="C", TZ="UTC")
                phase = run([str(tools[variant]), "-j8", "all"], build,
                            root / f"s{sample}-{variant}.log", env, root / "commands.jsonl")
                if os.read(fd, 64) != b"xx":
                    raise RuntimeError("FIFO tokens were lost or duplicated")
                for branch in branches:
                    for i in range(32):
                        if (build / branch / f"result-{i}").read_bytes() != b"built":
                            raise RuntimeError("Incorrect synthetic build artifact")
                if source_hashes != {str(p.relative_to(build)): digest(p) for p in build.rglob("Makefile")}:
                    raise RuntimeError("Synthetic Makefiles changed")
            finally:
                os.close(fd)
            rows.append(dict(project="synthetic", entrypoint="direct", jobs=8,
                             scenario="fifo_contention", sample=sample, warmup=sample == 0,
                             variant=variant, total_seconds=phase["seconds"], phase=phase))
            shutil.rmtree(build)
    report = dict(scope="Synthetic contention only; not release qualification",
                  protocol="one discarded warmup pair; seven alternating measured pairs",
                  binaries={v: digest(t) for v, t in tools.items()}, quota=2,
                  branches=16, recipes_per_branch=32, samples=rows,
                  harness_sha256=digest(Path(__file__)), summary=paired_summary(rows))
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
