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
    parser.add_argument("--baseline-revision", required=True)
    parser.add_argument("--candidate-revision", required=True)
    parser.add_argument("--project", choices=("ffmpeg", "linux", "synthetic"), required=True)
    parser.add_argument("--llvm-version", type=int, default=18,
                        help="external campaign's version-suffixed LLVM tools (default: 18)")
    parser.add_argument("--entrypoints", nargs="+", choices=("build", "legacy"),
                        default=["build", "legacy"])
    parser.add_argument("--jobs", type=int, nargs="+", default=[8, 16])
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    args = parser.parse_args()
    if args.samples < 1 or args.warmups < 0 or min(args.jobs) < 1 or args.llvm_version < 1:
        parser.error("positive samples/jobs and nonnegative warmups required")
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if (root / "metadata.json").exists():
        parser.error("use a fresh report directory; previous evidence is never overwritten")
    tools = {k: getattr(args, k).resolve() for k in ("baseline", "candidate")}
    env = dict(HOME=str(Path.home()), USER="benchmark", LOGNAME="benchmark",
               SHELL="/bin/sh", PATH="/usr/bin:/bin")
    env.update(CCACHE_DISABLE="1", LC_ALL="C", TZ="UTC", SOURCE_DATE_EPOCH="1731888000",
               KBUILD_BUILD_USER="benchmark", KBUILD_BUILD_HOST="benchmark",
               KBUILD_BUILD_VERSION="1", KBUILD_BUILD_TIMESTAMP="Mon Nov 18 00:00:00 UTC 2024")
    versions = {name: subprocess.check_output(command, env=env, text=True)
                for name, command in {"clang": [f"clang-{args.llvm_version}", "--version"],
                                      "ninja": ["ninja", "--version"]}.items()}
    metadata = dict(project=args.project, jobs=args.jobs, samples=args.samples,
                    llvm_version=args.llvm_version,
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
        archive = args.archives.resolve() / (version + ".tar.xz")
        metadata["archive_sha256"] = digest(archive)
        with tarfile.open(archive) as release:
            release.extractall(root / "sources", filter="data")
        source = root / "sources" / version
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    before_source = source_manifest(source) if source else {}
    rows = []
    journal = root / "commands.jsonl"
    failure = None
    try:
        for jobs in args.jobs:
            for entrypoint in args.entrypoints:
                for sample in range(args.warmups + args.samples):
                    order = ("baseline", "candidate") if sample % 2 == 0 else (
                        "candidate", "baseline")
                    hashes = {}
                    for variant in order:
                        build = root / f"{entrypoint}-j{jobs}-r{sample}-{variant}"
                        build.mkdir()
                        tool = str(tools[variant])
                        common = []
                        if args.project == "ffmpeg":
                            configure = [str(source / "configure"), f"--cc=clang-{args.llvm_version}",
                                         f"--cxx=clang++-{args.llvm_version}", f"--ld=clang-{args.llvm_version}",
                                         "--disable-autodetect", "--disable-doc", "--disable-debug"]
                            artifacts = ("ffmpeg", "ffprobe")
                            changed_source, missing = source / "fftools/ffmpeg.c", "fftools/ffmpeg.o"
                            unrelated = "libavutil/adler32.o"
                            configs = ("config.h", "config_components.h")
                        elif args.project == "linux":
                            common = ["-f", str(source / "Makefile"), f"LLVM=-{args.llvm_version}",
                                      "LLVM_IAS=1", "ARCH=x86"]
                            configure = [tool, *common, "defconfig"]
                            artifacts = ("vmlinux", "arch/x86/boot/bzImage")
                            changed_source, missing = source / "init/main.c", "init/main.o"
                            unrelated = "kernel/fork.o"
                            configs = (".config",)
                        else:
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
                        configuration = (run(configure, build, build / "configure.log", env, journal)
                                         if configure else None)
                        config_hash = {p: digest(build / p) for p in configs}
                        if hashes.get("configuration", config_hash) != config_hash:
                            raise RuntimeError("Baseline/candidate configurations differ")
                        hashes["configuration"] = config_hash
                        for scenario in ("clean", "noop", "source", "missing", "graph"):
                            previous_objects = objects(build)
                            timestamp = changed_source.stat()
                            try:
                                if scenario == "source":
                                    os.utime(changed_source, None)
                                elif scenario == "missing":
                                    (build / missing).unlink()
                                elif scenario == "graph":
                                    if args.project == "synthetic":
                                        (build / "config.mk").write_text("MODE := changed\n")
                                    else:
                                        os.utime(build / "Makefile", None)
                                phases = {}
                                start = time.perf_counter()
                                if entrypoint == "build":
                                    phases["invocation"] = run(
                                        [tool, "build", f"-j{jobs}", *common, "all"], build,
                                        build / f"{scenario}.log", env, journal)
                                else:
                                    phases["generation" if scenario == "clean" else "regeneration"] = run(
                                        [tool, "--ninja", "--regen", f"-j{jobs}", *common, "all"],
                                        build, build / f"{scenario}-regen.log", env, journal)
                                    phases["execution"] = run(
                                        ["sh", "./ninja.sh", f"-j{jobs}"], build,
                                        build / f"{scenario}.log", env, journal)
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
                                mode = "changed" if scenario == "graph" else "original"
                                for i in range(512):
                                    if (build / f"out{i}.o").read_text() != f"{mode}:input {i}\n":
                                        raise RuntimeError(f"Incorrect synthetic output out{i}.o")
                            hashes[scenario] = output_hashes
                            graph = build / (".openmktr/build.ninja" if entrypoint == "build" else "build.ninja")
                            row = dict(project=args.project, entrypoint=entrypoint, jobs=jobs,
                                       sample=sample, warmup=sample < args.warmups, variant=variant,
                                       scenario=scenario, total_seconds=elapsed, phases=phases,
                                       configuration=configuration, config_sha256=config_hash,
                                       artifacts=output_hashes, changed_objects=changed,
                                       graph_bytes=graph.stat().st_size)
                            rows.append(row)
                            with (root / "samples.jsonl").open("a") as output:
                                output.write(json.dumps(row) + "\n")
                            print(json.dumps({k: row[k] for k in (
                                "project", "entrypoint", "jobs", "sample", "warmup",
                                "variant", "scenario", "total_seconds")}), flush=True)
                        if args.project == "ffmpeg":
                            run([str(build / "ffmpeg"), "-v", "error", "-f", "lavfi", "-i",
                                 "testsrc2=size=64x64:rate=10", "-t", "0.2", "-c:v", "mpeg4",
                                 "-f", "null", "-"], build, build / "smoke.log", env, journal)
    except Exception as error:
        failure = str(error)
        raise
    finally:
        after_source = source_manifest(source) if source else {}
        changed = sorted(p for p in before_source.keys() | after_source.keys()
                         if before_source.get(p) != after_source.get(p))
        report = dict(metadata=metadata, failure=failure, source_changes=changed,
                      summary=paired_summary(rows))
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if changed:
            raise RuntimeError(f"Release source contents changed: {changed[:10]}")


if __name__ == "__main__":
    main()
