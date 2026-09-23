#!/usr/bin/env python3
"""Repeatable conversion, regeneration, and execution measurements on Linux."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def run(command: list[str], cwd: Path) -> tuple[float, int]:
    start = time.perf_counter()
    child = subprocess.Popen(command, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _, status, usage = os.wait4(child.pid, 0)
    child.returncode = os.waitstatus_to_exitcode(status)
    if child.returncode:
        raise RuntimeError(f"Command failed with status {child.returncode}: {command}")
    return time.perf_counter() - start, usage.ru_maxrss


def measure(binary: Path, repeats: int) -> dict:
    with tempfile.TemporaryDirectory(prefix="kati-bench-") as temp:
        cwd = Path(temp)
        lines = [f"VAR{i} := value{i}\n" for i in range(10000)]
        lines += [f"target{i}:\n\t@:\n" for i in range(10000)]
        lines += ["all: " + " ".join(f"target{i}" for i in range(10000)) + "\n"]
        (cwd / "Makefile").write_text("".join(lines), encoding="utf-8")
        convert = [str(binary), "--ninja", "-f", "Makefile", "all"]
        regen = [str(binary), "--ninja", "--regen", "-f", "Makefile", "all"]
        run(regen, cwd)
        results = {}
        for name, command in (("convert", convert), ("noop_regen", regen)):
            samples = [run(command, cwd) for _ in range(repeats + 2)][2:]
            results[name] = {
                "median_seconds": statistics.median(value[0] for value in samples),
                "peak_rss_kib": max(value[1] for value in samples),
            }

        recipes = [".PHONY: all\nall: " + " ".join(f"job{i}" for i in range(16)) + "\n"]
        recipes += [f"job{i}:\n\t@sleep 0.05; touch $@\n" for i in range(16)]
        (cwd / "Makefile").write_text("".join(recipes), encoding="utf-8")
        execution = []
        for _ in range(repeats + 2):
            for output in cwd.glob("job[0-9]*"):
                output.unlink()
            execution.append(run([str(binary), "-j4", "-f", "Makefile", "all"], cwd))
        execution = execution[2:]
        results["parallel_exec"] = {
            "median_seconds": statistics.median(value[0] for value in execution),
            "peak_rss_kib": max(value[1] for value in execution),
        }
        results["binary_bytes"] = binary.stat().st_size
        return results


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    binary = args.binary.resolve()
    report = {"current": measure(binary, args.repeats)}
    if args.baseline:
        report["baseline"] = measure(args.baseline.resolve(), args.repeats)
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
