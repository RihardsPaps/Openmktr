#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Paired Linux Ninja benchmarks; external release tools are campaign-only.

Never edits release source contents. Each clean sample has a fresh build tree;
incremental samples follow that build. Tracing is a separate diagnostic run.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import resource
import statistics
import shutil
import subprocess
import tarfile
import time


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_manifest(root):
    return {str(p.relative_to(root)): ("link:" + os.readlink(p) if p.is_symlink()
                                     else digest(p))
            for p in sorted(root.rglob("*")) if p.is_symlink() or p.is_file()}


def objects(root):
    return {str(p.relative_to(root)): (p.stat().st_mtime_ns, p.stat().st_size)
            for p in root.rglob("*.o")}


def effective_settings(options):
    profile = "safe"
    disabled = set()
    for option in options:
        if option.startswith("--graph_optimizations="):
            profile = option.split("=", 1)[1]
        elif option.startswith("--disable_graph_optimization="):
            disabled.add(option.split("=", 1)[1])
    features = {feature: profile != "off" and feature not in disabled and
                (profile == "aggressive" or feature not in ("recursive", "monitor"))
                for feature in ("evaluation", "pattern_index", "resolution", "includes", "recursive", "compact", "monitor")}
    return dict(profile=profile, enabled=features,
                general_state_replay=profile == "aggressive" and features["evaluation"],
                monitor_policy="already-running-only")


def paired_summary(rows):
    pairs = {}
    for row in rows:
        if row["warmup"]:
            continue
        key = (row["project"], row["entrypoint"], row["jobs"], row["scenario"])
        pairs.setdefault(key, {}).setdefault(row["sample"], {})[row["variant"]] = row
    result = []
    for key, samples in sorted(pairs.items()):
        complete = [p for p in samples.values() if set(p) == {"baseline", "candidate"}]
        if not complete:
            continue
        gains = [100 * (1 - p["candidate"]["total_seconds"] /
                        p["baseline"]["total_seconds"]) for p in complete]
        rng = random.Random(0)
        boot = sorted(statistics.median(rng.choices(gains, k=len(gains)))
                      for _ in range(10000))
        result.append(dict(zip(("project", "entrypoint", "jobs", "scenario"), key),
                           pairs=len(complete), paired_improvement_percent=gains,
                           median_improvement_percent=statistics.median(gains),
                           bootstrap_95_percent=[boot[249], boot[9749]],
                           supported_improvement=len(gains) >= 7 and boot[249] > 0,
                           supported_regression=len(gains) >= 7 and boot[9749] < 0,
                           baseline_median_seconds=statistics.median(
                               p["baseline"]["total_seconds"] for p in complete),
                           candidate_median_seconds=statistics.median(
                               p["candidate"]["total_seconds"] for p in complete)))
    return result


def invocation_commands(tool, entrypoint, jobs, common, options=(), kind="omktr"):
    """The timed invocation includes every graph validation/generation step."""
    arguments = [f"-j{jobs}", *common, "all"]
    if kind == "make" or entrypoint == "direct":
        return [("invocation", [tool, *arguments, *options])]
    if entrypoint == "build":
        return [("invocation", [tool, "build", *arguments, *options])]
    return [("generation", [tool, "--ninja", "--regen", *arguments, *options]),
            ("execution", ["sh", "./ninja.sh", f"-j{jobs}"])]


def parity_summary(rows):
    """Incomplete, failed or under-sampled cells can never qualify parity."""
    results = []
    for cell in paired_summary(rows):
        gains = cell["paired_improvement_percent"]
        ratios = [1 - gain / 100 for gain in gains]
        upper = 1 - cell["bootstrap_95_percent"][0] / 100
        results.append({**cell, "median_ratio": statistics.median(ratios),
                        "ratio_upper_95": upper,
                        "parity_passed": cell["pairs"] >= 7 and
                        statistics.median(ratios) <= 1 and upper <= 1.05})
    return results


def memory_summary(rows):
    """Per-command wait4 peak process RSS, paired separately from wall time."""
    converted = []
    for row in rows:
        peak = row.get("peak_rss_kib", 0)
        if peak > 0:
            converted.append({**row, "total_seconds": peak})
    results = parity_summary(converted)
    for cell in results:
        cell["metric"] = "wait4_peak_process_rss_kib"
        cell["baseline_median_rss_kib"] = cell.pop("baseline_median_seconds")
        cell["candidate_median_rss_kib"] = cell.pop("candidate_median_seconds")
    return results


def no_regression_passed(timing, memory, expected_cells, samples):
    """Every requested cell must have complete timing and memory evidence."""
    return expected_cells > 0 and samples >= 7 and all(
        len(cells) == expected_cells and
        all(cell["pairs"] == samples and cell["parity_passed"] for cell in cells)
        for cells in (timing, memory))


def run(command, cwd, log, env, journal):
    start = time.perf_counter()
    with log.open("wb") as output:
        child = subprocess.Popen(command, cwd=cwd, env=env, stdout=output,
                                 stderr=subprocess.STDOUT)
        _, status, usage = os.wait4(child.pid, 0)
        child.returncode = os.waitstatus_to_exitcode(status)
    record = dict(command=list(map(str, command)), cwd=str(cwd), log=str(log),
                  status=child.returncode, seconds=time.perf_counter() - start,
                  user_seconds=usage.ru_utime, system_seconds=usage.ru_stime,
                  peak_rss_kib=usage.ru_maxrss)
    with journal.open("a") as output:
        output.write(json.dumps(record) + "\n")
    if child.returncode:
        raise RuntimeError(f"Command failed ({child.returncode}): {log}")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--archives", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline-option", action="append", default=[],
                        help="extra baseline generation option, for same-binary feature ablations")
    parser.add_argument("--candidate-option", action="append", default=[],
                        help="extra candidate generation option; repeat with =--OPTION")
    parser.add_argument("--ninja-dir", type=Path,
                        help="directory containing the Ninja executable for both variants")
    parser.add_argument("--toolchain-dir", type=Path,
                        help="private external LLVM executable directory for both variants")
    parser.add_argument("--monitor", action="store_true",
                        help="explicitly start a candidate monitor; include startup in clean timing")
    parser.add_argument("--retained", action="store_true",
                        help="reuse separately initialized build trees for incremental-only ablations; never reports clean timings")
    parser.add_argument("--baseline-kind", choices=("omktr", "make"), default="omktr",
                        help="use external GNU Make as the baseline for parity qualification")
    parser.add_argument("--release-version",
                        help="archive directory/version; must occur in dependency-campaign.json")
    parser.add_argument("--baseline-revision", required=True)
    parser.add_argument("--candidate-revision", required=True)
    parser.add_argument("--project", choices=("ffmpeg", "linux", "synthetic"), required=True)
    parser.add_argument("--llvm-version", type=int,
                        help="optional version suffix for historical LLVM tools; default: distribution clang")
    parser.add_argument("--entrypoints", nargs="+", choices=("build", "legacy", "direct"),
                        default=["build", "legacy"])
    parser.add_argument("--jobs", type=int, nargs="+", default=[8, 16])
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--require-no-regression", action="store_true",
                        help="exit 3 unless every timing and memory cell passes against the supplied baseline")
    args = parser.parse_args()
    if (args.samples < 1 or args.warmups < 0 or min(args.jobs) < 1 or
            (args.llvm_version is not None and args.llvm_version < 1)):
        parser.error("positive samples/jobs and nonnegative warmups required")
    if args.baseline_kind == "make" and args.baseline_option:
        parser.error("--baseline-option is reserved for Openmktr comparisons")
    if args.monitor and "direct" in args.entrypoints:
        parser.error("--monitor cannot be used with direct execution")
    if args.monitor and not effective_settings(args.candidate_option)["enabled"]["monitor"]:
        parser.error("--monitor requires candidate aggressive mode with monitoring enabled")
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if (root / "metadata.json").exists():
        parser.error("use a fresh report directory; previous evidence is never overwritten")
    tools = {k: getattr(args, k).resolve() for k in ("baseline", "candidate")}
    env = dict(HOME=str(Path.home()), USER="benchmark", LOGNAME="benchmark",
               SHELL="/bin/sh", PATH="/usr/bin:/bin")
    if args.toolchain_dir:
        env["PATH"] = str(args.toolchain_dir.resolve()) + ":" + env["PATH"]
    if args.ninja_dir:
        env["PATH"] = str(args.ninja_dir.resolve()) + ":" + env["PATH"]
    env.update(CCACHE_DISABLE="1", LC_ALL="C", TZ="UTC", SOURCE_DATE_EPOCH="1731888000",
               KBUILD_BUILD_USER="benchmark", KBUILD_BUILD_HOST="benchmark",
               KBUILD_BUILD_VERSION="1", KBUILD_BUILD_TIMESTAMP="Mon Nov 18 00:00:00 UTC 2024")
    suffix = f"-{args.llvm_version}" if args.llvm_version is not None else ""
    versions = {name: subprocess.check_output(command, env=env, text=True)
                for name, command in {"clang": [f"clang{suffix}", "--version"],
                                      "ninja": ["ninja", "--version"]}.items()}
    metadata = dict(project=args.project, baseline_kind=args.baseline_kind, jobs=args.jobs, samples=args.samples,
                    build_state="retained incremental only" if args.retained else "fresh clean per pair",
                    candidate_options=args.candidate_option,
                    baseline_options=args.baseline_option,
                    candidate_effective_settings=effective_settings(args.candidate_option),
                    baseline_effective_settings=effective_settings(args.baseline_option)
                    if args.baseline_option or tools["baseline"] == tools["candidate"]
                    else "starting revision defaults",
                    monitor_explicitly_started=args.monitor,
                    monitor_startup_timing=("unmeasured initialization; cost retained in command journal"
                                           if args.retained else "included in candidate clean invocation")
                        if args.monitor else None,
                    ninja_directory=str(args.ninja_dir.resolve()) if args.ninja_dir else None,
                    ninja_bundle_sha256={p.name: digest(p) for p in sorted(args.ninja_dir.resolve().iterdir())
                                         if p.is_file()} if args.ninja_dir else None,
                    ninja_executable={"path": shutil.which("ninja", path=env["PATH"]),
                                      "sha256": digest(Path(shutil.which("ninja", path=env["PATH"])))},
                    llvm_version=args.llvm_version,
                    toolchain_directory=str(args.toolchain_dir.resolve()) if args.toolchain_dir else None,
                    toolchain_executable_sha256={p.name: digest(p) for p in
                        sorted(args.toolchain_dir.resolve().iterdir()) if p.is_file()}
                        if args.toolchain_dir else None,
                    toolchain_manifest_sha256=digest(args.toolchain_dir.resolve().parent / "manifest.json")
                        if args.toolchain_dir and
                        (args.toolchain_dir.resolve().parent / "manifest.json").is_file() else None,
                    execution_image=os.environ.get("OMKTR_BENCHMARK_IMAGE_ID"),
                    stack_limit_bytes=resource.getrlimit(resource.RLIMIT_STACK),
                    platform=list(os.uname()),
                    warmups=args.warmups, entrypoints=args.entrypoints,
                    harness_sha256=digest(Path(__file__)),
                    cpu_affinity=sorted(os.sched_getaffinity(0)), versions=versions,
                    binaries={k: dict(path=str(v), sha256=digest(v),
                                      revision=getattr(args, k + "_revision"),
                                      version=subprocess.check_output(
                                          [str(v), "--version"], text=True, env=env))
                              for k, v in tools.items()},
                    environment={k: env[k] for k in (
                        "PATH", "CCACHE_DISABLE", "LC_ALL", "TZ", "SOURCE_DATE_EPOCH",
                        "KBUILD_BUILD_USER", "KBUILD_BUILD_HOST", "KBUILD_BUILD_VERSION",
                        "KBUILD_BUILD_TIMESTAMP")})
    source = None
    if args.project != "synthetic":
        if args.archives is None:
            parser.error("--archives is required for release builds")
        version = "ffmpeg-7.1.1" if args.project == "ffmpeg" else "linux-6.12"
        expected_digest = None
        if args.release_version:
            campaign = json.loads(Path(__file__).with_name("dependency-campaign.json").read_text())
            selected = next((item for item in campaign["components"]
                             if item["id"] == args.project and
                             item["version"] == args.release_version), None)
            if selected is None:
                parser.error("--release-version must match the reviewed campaign manifest")
            version = selected.get("directory", args.project + "-" + args.release_version)
            expected_digest = selected["sha256"]
        archive = args.archives.resolve() / (version + ".tar.xz")
        metadata["archive_sha256"] = digest(archive)
        if expected_digest and metadata["archive_sha256"] != expected_digest:
            parser.error("release archive checksum differs from the campaign manifest")
        metadata["release_directory"] = version
        with tarfile.open(archive) as release:
            release.extractall(root / "sources", filter="data")
        source = root / "sources" / version
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    before_source = source_manifest(source) if source else {}
    rows = []
    journal = root / "commands.jsonl"
    failure = None
    monitors = []
    try:
        for jobs in args.jobs:
            for entrypoint in args.entrypoints:
                for sample in range(args.warmups + args.samples):
                    order = ("baseline", "candidate") if sample % 2 == 0 else (
                        "candidate", "baseline")
                    hashes = {}
                    for variant in order:
                        extra_options = getattr(args, variant + "_option")
                        build = root / (f"{entrypoint}-j{jobs}-retained-{variant}" if args.retained
                                        else f"{entrypoint}-j{jobs}-r{sample}-{variant}")
                        initialize = not build.exists()
                        if initialize:
                            build.mkdir()
                        logs = build / ".benchmark-logs" / str(sample) if args.retained else build
                        logs.mkdir(parents=True, exist_ok=True)
                        tool = str(tools[variant])
                        kind = args.baseline_kind if variant == "baseline" else "omktr"
                        common = []
                        if args.project == "ffmpeg":
                            configure = [str(source / "configure"), f"--cc=clang{suffix}",
                                         f"--cxx=clang++{suffix}", f"--ld=clang{suffix}",
                                         "--disable-autodetect", "--disable-doc", "--disable-debug"]
                            artifacts = ("ffmpeg", "ffprobe")
                            changed_source, missing = source / "fftools/ffmpeg.c", "fftools/ffmpeg.o"
                            unrelated = "libavutil/adler32.o"
                            configs = ("config.h", "config_components.h")
                        elif args.project == "linux":
                            common = ["-f", str(source / "Makefile"), f"LLVM={suffix or '1'}",
                                      "LLVM_IAS=1", "ARCH=x86"]
                            configure = [tool, *common, "defconfig"]
                            artifacts = ("vmlinux", "arch/x86/boot/bzImage")
                            changed_source, missing = source / "init/main.c", "init/main.o"
                            unrelated = "kernel/fork.o"
                            configs = (".config",)
                        else:
                            if initialize:
                                (build / "config.mk").write_text("MODE := original\n")
                                (build / "Makefile").write_text(
                                    "include config.mk\n.PHONY: all\nall: " +
                                    " ".join(f"out{i}.o" for i in range(512)) + "\n" +
                                    "".join(f"out{i}.o: src{i} config.mk\n"
                                            f"\t@printf '$(MODE):' > $@; cat $< >> $@\n"
                                            for i in range(512)))
                                for i in range(512):
                                    (build / f"src{i}").write_text(f"input {i}\n")
                            configure = None
                            artifacts = ("out0.o", "out511.o")
                            changed_source, missing = build / "src0", "out0.o"
                            unrelated = "out511.o"
                            configs = ("config.mk",)
                        configuration = (run(configure, build, logs / "configure.log", env, journal)
                                         if configure and initialize else None)
                        config_hash = {p: digest(build / p) for p in configs}
                        if hashes.get("configuration", config_hash) != config_hash:
                            raise RuntimeError("Baseline/candidate configurations differ")
                        hashes["configuration"] = config_hash
                        if args.retained:
                            if initialize:
                                if variant == "candidate" and args.monitor:
                                    run([tool, "monitor", "start"], build,
                                        logs / "setup-monitor-start.log", env, journal)
                                    monitors.append((tool, build))
                                for phase, command in invocation_commands(
                                        tool, entrypoint, jobs, common, extra_options, kind):
                                    log_name = "setup-generation.log" if phase == "generation" else "setup-clean.log"
                                    run(command, build, logs / log_name, env, journal)
                            setup_hashes = {p: digest(build / p) for p in artifacts}
                            if hashes.get("clean", setup_hashes) != setup_hashes:
                                raise RuntimeError("Retained baseline/candidate setup artifacts differ")
                            hashes["clean"] = setup_hashes
                        for scenario in (("noop", "source", "missing", "graph") if args.retained
                                         else ("clean", "noop", "source", "missing", "graph")):
                            previous_objects = objects(build)
                            timestamp = changed_source.stat()
                            try:
                                if scenario == "source":
                                    os.utime(changed_source, None)
                                elif scenario == "missing":
                                    (build / missing).unlink()
                                elif scenario == "graph":
                                    if args.project == "synthetic" and not args.retained:
                                        (build / "config.mk").write_text("MODE := changed\n")
                                    elif args.project == "synthetic":
                                        os.utime(build / "config.mk", None)
                                    else:
                                        os.utime(build / "Makefile", None)
                                phases = {}
                                start = time.perf_counter()
                                if scenario == "clean" and variant == "candidate" and args.monitor:
                                    phases["monitor_startup"] = run(
                                        [tool, "monitor", "start"], build,
                                        logs / "monitor-start.log", env, journal)
                                    monitors.append((tool, build))
                                for phase, command in invocation_commands(
                                        tool, entrypoint, jobs, common, extra_options, kind):
                                    if phase == "generation" and scenario != "clean":
                                        phase = "regeneration"
                                    log_name = f"{scenario}-regen.log" if phase in ("generation", "regeneration") else f"{scenario}.log"
                                    phases[phase] = run(command, build,
                                        logs / log_name, env, journal)
                                elapsed = time.perf_counter() - start
                            finally:
                                if scenario == "source":
                                    os.utime(changed_source, ns=(timestamp.st_atime_ns,
                                                               timestamp.st_mtime_ns))
                            current_objects = objects(build)
                            changed = sorted(p for p in previous_objects.keys() | current_objects.keys()
                                             if previous_objects.get(p) != current_objects.get(p))
                            # Baseline extra work is a measurable performance
                            # defect, not permission for the candidate to do it.
                            if scenario == "noop" and changed and variant == "candidate":
                                raise RuntimeError(f"No-op rebuilt objects: {changed[:10]}")
                            if scenario in ("source", "missing") and missing not in changed:
                                raise RuntimeError(f"{scenario} failed to rebuild {missing}")
                            if scenario in ("source", "missing") and unrelated in changed:
                                raise RuntimeError(f"{scenario} rebuilt unrelated {unrelated}")
                            output_hashes = {p: digest(build / p) for p in artifacts}
                            if hashes.get(scenario, output_hashes) != output_hashes:
                                raise RuntimeError(f"Artifact mismatch: {variant}/{scenario}")
                            if args.project != "synthetic" and scenario != "clean" and output_hashes != hashes["clean"]:
                                raise RuntimeError(f"{scenario} changed deterministic release artifacts")
                            if args.project == "synthetic":
                                mode = "changed" if scenario == "graph" and not args.retained else "original"
                                for i in range(512):
                                    if (build / f"out{i}.o").read_text() != f"{mode}:input {i}\n":
                                        raise RuntimeError(f"Incorrect synthetic output out{i}.o")
                            hashes[scenario] = output_hashes
                            graph = build / (".openmktr/build.ninja" if entrypoint == "build" else "build.ninja")
                            row = dict(project=args.project, entrypoint=entrypoint, jobs=jobs,
                                       sample=sample, warmup=sample < args.warmups, variant=variant,
                                       scenario=scenario, total_seconds=elapsed, phases=phases,
                                       peak_rss_kib=max((phase.get("peak_rss_kib", 0) for phase in phases.values()), default=0),
                                       configuration=configuration, config_sha256=config_hash,
                                       artifacts=output_hashes, changed_objects=changed,
                                       graph_bytes=graph.stat().st_size if graph.exists() else 0)
                            rows.append(row)
                            with (root / "samples.jsonl").open("a") as output:
                                output.write(json.dumps(row) + "\n")
                            print(json.dumps({k: row[k] for k in (
                                "project", "entrypoint", "jobs", "sample", "warmup",
                                "variant", "scenario", "total_seconds")}), flush=True)
                        if args.project == "ffmpeg":
                            run([str(build / "ffmpeg"), "-v", "error", "-f", "lavfi", "-i",
                                 "testsrc2=size=64x64:rate=10", "-t", "0.2", "-c:v", "mpeg4",
                                 "-f", "null", "-"], build, logs / "smoke.log", env, journal)
                        if not args.retained and (tool, build) in monitors:
                            run([tool, "monitor", "stop"], build,
                                build / "monitor-stop.log", env, journal)
                            monitors.remove((tool, build))
    except Exception as error:
        failure = str(error)
        raise
    finally:
        cleanup_errors = []
        for tool, build in monitors:
            try:
                with (build / "monitor-cleanup.log").open("wb") as output:
                    subprocess.run([tool, "monitor", "stop"], cwd=build, env=env,
                                   stdout=output, stderr=subprocess.STDOUT, timeout=10, check=True)
            except (OSError, subprocess.SubprocessError) as error:
                cleanup_errors.append(f"{build}: {error}")
        after_source = source_manifest(source) if source else {}
        changed = sorted(p for p in before_source.keys() | after_source.keys()
                         if before_source.get(p) != after_source.get(p))
        timing = parity_summary(rows)
        report = dict(metadata=metadata, failure=failure or ("monitor cleanup failed" if cleanup_errors else None),
                      monitor_cleanup_errors=cleanup_errors, source_changes=changed,
                      summary=paired_summary(rows),
                      parity=timing if args.baseline_kind == "make" else None,
                      memory=memory_summary(rows))
        expected_cells = len(args.jobs) * len(args.entrypoints) * (4 if args.retained else 5)
        report["no_regression"] = timing
        report["no_regression_passed"] = (
            failure is None and not cleanup_errors and not changed and
            no_regression_passed(report["no_regression"], report["memory"],
                                 expected_cells, args.samples))
        report["parity_passed"] = (args.baseline_kind == "make" and
                                   report["no_regression_passed"])
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if changed:
            raise RuntimeError(f"Release source contents changed: {changed[:10]}")
        if cleanup_errors and failure is None:
            raise RuntimeError("Monitor cleanup failed; see retained report")
    if args.require_no_regression and not report["no_regression_passed"]:
        raise SystemExit(3)


if __name__ == "__main__":
    main()
