#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Investigate child graph reuse without changing recursive build semantics.

Compare direct children with explicitly opted-in `omktr build` children below
the same generated Ninja parent. All workloads live in disposable directories.
Recipe occupancy is a lower bound on slot utilization, not FIFO telemetry.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time


STAT = re.compile(r"^\*kati\*: (eval time|parse file time): ([0-9.]+)(?: / (\d+))?$",
                  re.MULTILINE)


def counters(log):
    result = {"evaluation_passes": 0, "parse_seconds": 0.0,
              "parsed_files": 0, "evaluation_seconds": 0.0,
              "graph_reuses": (log.count("No need to regenerate ninja file") +
                               log.count("Recursive cache hit"))}
    for name, elapsed, count in STAT.findall(log):
        if name == "eval time":
            result["evaluation_passes"] += 1
            result["evaluation_seconds"] += float(elapsed)
        else:
            result["parse_seconds"] += float(elapsed)
            result["parsed_files"] += int(count)
    return result


def occupancy(intervals, elapsed, jobs):
    events = [(start, 1) for start, end in intervals]
    events += [(end, -1) for start, end in intervals]
    active = peak = 0
    for _, change in sorted(events):
        active += change
        peak = max(peak, active)
    busy = sum(end - start for start, end in intervals)
    return {"peak_active_recipes": peak,
            "recipe_seconds": busy,
            "recipe_slot_occupancy": busy / (elapsed * jobs)}


def run(command, cwd, jobs, env):
    start = time.perf_counter()
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                            text=True, timeout=120)
    elapsed = time.perf_counter() - start
    log = result.stdout + result.stderr
    if result.returncode:
        raise RuntimeError(f"{command} failed ({result.returncode}):\n{log}")
    intervals = [json.loads(path.read_text())
                 for path in cwd.glob("child*/interval*.json")]
    for path in cwd.glob("child*/interval*.json"):
        path.unlink()
    slots = occupancy(intervals, elapsed, jobs)
    return {"wall_seconds": elapsed, **counters(log),
            "recipes_run": len(intervals),
            "job_limit_exceeded": slots["peak_active_recipes"] > jobs,
            **slots}


WORKER = '''import json, sys, time
from pathlib import Path
start = time.monotonic()
time.sleep(float(sys.argv[2]))
Path(sys.argv[1]).write_text(sys.argv[3])
end = time.monotonic()
Path("interval" + sys.argv[1] + ".json").write_text(json.dumps([start, end]))
'''


def prepare(cwd, children, variables, recipes, delay, cached, cache_kind="native"):
    # MAKE remains the project's normal executable path. Only this synthetic
    # fixture explicitly chooses the Ninja child workflow.
    names = [f"child{i}" for i in range(children)]
    lines = [".PHONY: all " + " ".join(names) + "\n",
             "all: " + " ".join(names) + "\n"]
    for name in names:
        option = ("build " if cache_kind == "ninja" else "--recursive_cache ") if cached else ""
        lines.append(f"{name}:\n\t@$(MAKE) {option}"
                     f"--kati_stats -C {name} all MODE=$(MODE)\n")
        child = cwd / name
        child.mkdir()
        (child / "worker.py").write_text(WORKER)
        (child / "config.mk").write_text("CONFIG := initial\n")
        (child / "input").write_text("input\n")
        source = [f"VAR{i} := value{i}\n" for i in range(variables)]
        source += ["include config.mk\n", "all: " + " ".join(
            f"out{i}" for i in range(recipes)) + "\n"]
        source += [f"out{i}: input\n\t@python3 worker.py $@ {delay} "
                   "'$(CONFIG):$(MODE):$(BENCH_VALUE)'\n"
                   for i in range(recipes)]
        (child / "Makefile").write_text("".join(source))
    (cwd / "Makefile").write_text("".join(lines))


def check_outputs(cwd, children, recipes, expected):
    for child in range(children):
        for recipe in range(recipes):
            path = cwd / f"child{child}/out{recipe}"
            if path.read_text() != expected:
                raise RuntimeError(f"{path}: expected {expected!r}, got {path.read_text()!r}")


def check_sample(sample, children, recipes, reused=False, noop=False):
    expected = 0 if reused else children
    if sample["evaluation_passes"] != expected:
        raise RuntimeError(f"Expected {expected} child evaluations: {sample}")
    if sample["graph_reuses"] != (children if reused else 0):
        raise RuntimeError(f"Unexpected child cache reuse count: {sample}")
    # Preserve oversubscription evidence rather than losing the measurements.
    # An observed violation disqualifies performance claims for that setup.
    if noop and sample["recipe_seconds"]:
        raise RuntimeError(f"No-op build ran recipes: {sample}")
    if sample["recipes_run"] != (0 if noop else children * recipes):
        raise RuntimeError(f"Unexpected recipe execution count: {sample}")


def summarize(samples):
    # Keep raw samples: medians alone hide regeneration and scheduling outliers.
    return {"median": {key: statistics.median(sample[key] for sample in samples)
                       for key in samples[0]}, "samples": samples}


def measure(binary, args, cached, jobs):
    with tempfile.TemporaryDirectory(prefix="omktr-recursive-bench-") as temp:
        cwd = Path(temp)
        prepare(cwd, args.children, args.variables, args.recipes, args.delay,
                cached, args.cache_kind)
        # Do not inherit a caller's jobserver, cached runtime state or MAKEFLAGS.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("KATI_") and key not in
               ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL")}
        env["BENCH_VALUE"] = "one"
        # The parent graph is deliberately excluded from timings/statistics.
        generation = [str(binary), "--ninja", f"-j{jobs}", "all", "MODE=one"]
        run(generation, cwd, jobs, env)
        command = ["sh", "./ninja.sh", f"-j{jobs}"]
        cold = run(command, cwd, jobs, env)
        check_sample(cold, args.children, args.recipes)
        check_outputs(cwd, args.children, args.recipes, "initial:one:one")
        noops = [run(command, cwd, jobs, env) for _ in range(args.repeats + 1)][1:]
        for sample in noops:
            check_sample(sample, args.children, args.recipes,
                         reused=cached, noop=True)
        work = []
        for _ in range(args.repeats + 1):
            for path in cwd.glob("child*/out[0-9]*"):
                path.unlink()
            work.append(run(command, cwd, jobs, env))
            check_sample(work[-1], args.children, args.recipes,
                         reused=cached and args.cache_kind == "ninja")
            check_outputs(cwd, args.children, args.recipes, "initial:one:one")
        # Change each configuration input separately so one invalidation cannot
        # mask a missing dependency. Remove outputs to compare equivalent work:
        # unlike Ninja, direct mode does not track changes in recipe text.
        changed = {}
        for change in ("include", "arguments", "environment", "recipe_input"):
            for path in cwd.glob("child*/out[0-9]*"):
                path.unlink()
            if change == "include":
                for path in cwd.glob("child*/config.mk"):
                    path.write_text("CONFIG := changed\n")
            elif change == "arguments":
                run([*generation[:-1], "MODE=two"], cwd, jobs, env)
            elif change == "environment":
                env["BENCH_VALUE"] = "two"
                # A legacy parent launcher deliberately restores its captured
                # environment. Refresh it outside the child measurement.
                run([*generation[:-1], "MODE=two"], cwd, jobs, env)
            else:
                for path in cwd.glob("child*/input"):
                    path.write_text("changed input\n")
            sample = run(command, cwd, jobs, env)
            check_sample(sample, args.children, args.recipes,
                         reused=cached and args.cache_kind == "ninja" and
                         change == "recipe_input")
            expected = {"include": "changed:one:one",
                        "arguments": "changed:two:one",
                        "environment": "changed:two:two",
                        "recipe_input": "changed:two:two"}[change]
            check_outputs(cwd, args.children, args.recipes, expected)
            changed[change] = sample
        return {"cold": cold, "warm_noop": summarize(noops),
                "warm_work": summarize(work[1:]), "invalidation_checks": changed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--cache-kind", choices=("ninja", "native"), default="native")
    parser.add_argument("--children", type=int, default=8)
    parser.add_argument("--variables", type=int, default=10000)
    parser.add_argument("--recipes", type=int, default=4)
    parser.add_argument("--delay", type=float, default=0.03)
    parser.add_argument("--jobs", type=int, nargs="+",
                        default=sorted({1, 4, len(os.sched_getaffinity(0))}))
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    if min(args.children, args.variables, args.recipes, args.repeats, *args.jobs) < 1:
        parser.error("workload sizes, job counts and repeats must be positive")
    if not 0 <= args.delay <= 1:
        parser.error("--delay must be between zero and one second")
    report = {"schema_version": 1, "platform": platform.platform(),
              "logical_cpus": os.cpu_count(),
              "cpu_affinity": sorted(os.sched_getaffinity(0)),
              "python": sys.version, "workload": vars(args).copy(),
              "notes": [f"Synthetic explicit {args.cache_kind} child-cache opt-in.",
                        "Child stats enabled; parent generation excluded.",
                        "One warmup discarded for each warm measurement.",
                        "Occupancy measures worker intervals, not token wait/hold time.",
                        "No-op means no child recipes; phony parent recursion still runs."]}
    report["workload"]["binary"] = str(args.binary.resolve())
    report["workload"]["baseline"] = str(args.baseline.resolve()) if args.baseline else None
    for label, binary in (("current", args.binary), ("baseline", args.baseline)):
        if binary is None:
            continue
        binary = binary.resolve()
        report[label] = {"binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                         "version": subprocess.check_output([str(binary), "--version"],
                                                            text=True).strip(),
                         "runs": {}}
        for jobs in args.jobs:
            modes = (False,) if label == "baseline" and args.cache_kind == "native" else (False, True)
            for cached in modes:
                name = f"{'cached' if cached else 'direct'}_j{jobs}"
                print(f"Measuring {label} {name}", file=sys.stderr, flush=True)
                report[label]["runs"][name] = measure(binary, args, cached, jobs)
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
