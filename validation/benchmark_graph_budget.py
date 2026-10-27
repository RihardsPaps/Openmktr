#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Bounded, descriptive FFmpeg/Linux comparisons; never statistical acceptance.

The fixed matrix uses 18 commands: two shared configurations, eight fresh
clean builds and eight incremental invocations. Configurations are cloned before
release compilation, so both variants start with identical prepared files.
An interrupted or failed command consumes its slot. No retries run implicitly.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import time

from benchmark_ninja import digest, objects, paired_summary, run, source_manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--toolchain-dir", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--used-builds", type=int, default=0,
                        help="commands already started under the user's budget")
    args = parser.parse_args()
    if not 0 <= args.used_builds or not 1 <= args.limit <= 20:
        parser.error("nonnegative used builds and a limit between 1 and 20 required")
    if args.used_builds + 18 > args.limit:
        parser.error("the fixed matrix needs 18 available command slots")
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    tools = {name: getattr(args, name).resolve() for name in ("baseline", "candidate")}
    env = dict(HOME="/root", USER="benchmark", LOGNAME="benchmark", SHELL="/bin/sh",
               PATH=str(args.toolchain_dir.resolve()) + ":/usr/bin:/bin",
               CCACHE_DISABLE="1", LC_ALL="C", TZ="UTC", SOURCE_DATE_EPOCH="1731888000",
               KBUILD_BUILD_USER="benchmark", KBUILD_BUILD_HOST="benchmark",
               KBUILD_BUILD_VERSION="1", KBUILD_BUILD_TIMESTAMP="Mon Nov 18 00:00:00 UTC 2024")
    plan = (("ffmpeg", 8, ("clean", "noop", "source")),
            ("linux", 16, ("clean", "noop", "source")),
            ("ffmpeg", 16, ("clean",)), ("linux", 8, ("clean",)))
    metadata = dict(limit=args.limit, previously_used=args.used_builds,
                    planned_commands=18, matrix=plan, environment=env,
                    platform=list(os.uname()), affinity=sorted(os.sched_getaffinity(0)),
                    compiler=subprocess.check_output(["clang-19", "--version"], env=env, text=True),
                    ninja=subprocess.check_output(["ninja", "--version"], env=env, text=True),
                    binaries={name: dict(path=str(path), sha256=digest(path)) for name, path in tools.items()},
                    candidate_settings="safe defaults; no implicitly started monitor",
                    protocol="One descriptive pair per selected scenario, no warmups or confidence claim; user capped the campaign at 20 commands.",
                    configuration="One shared configuration per project, copied before release compilation; configuration commands count toward the cap.",
                    harness_sha256=digest(Path(__file__)), archives={})
    sources, before_sources, templates, configs = {}, {}, {}, {}
    journal = root / "commands.jsonl"
    budget_log = root / "budget.jsonl"
    used = args.used_builds
    rows, smoke, failures = [], [], []
    release_artifacts = {}

    def execute(command, cwd, log, purpose):
        nonlocal used
        if used >= args.limit:
            raise RuntimeError("Build command budget exhausted; no process started")
        used += 1
        reservation = dict(slot=used, purpose=purpose, command=list(map(str, command)),
                           cwd=str(cwd), log=str(log), reserved_unix=time.time())
        with budget_log.open("a") as output:
            output.write(json.dumps(reservation) + "\n")
        print(json.dumps(dict(event="start", **reservation)), flush=True)
        record = run(command, cwd, log, env, journal)
        print(json.dumps(dict(event="finish", slot=used, purpose=purpose,
                              seconds=record["seconds"], status=record["status"])), flush=True)
        return record

    def common(project):
        if project == "linux":
            return ["-f", str(sources[project] / "Makefile"), "LLVM=-19", "LLVM_IAS=1", "ARCH=x86"]
        return []

    try:
        for project, version in (("ffmpeg", "ffmpeg-7.1.1"), ("linux", "linux-6.12")):
            archive = args.archives.resolve() / (version + ".tar.xz")
            metadata["archives"][project] = dict(path=str(archive), sha256=digest(archive))
            with tarfile.open(archive) as release:
                release.extractall(root / "sources", filter="data")
            source = sources[project] = root / "sources" / version
            before_sources[project] = source_manifest(source)
            template = templates[project] = root / (project + "-configuration")
            template.mkdir()
            if project == "ffmpeg":
                configure = [str(source / "configure"), "--cc=clang-19", "--cxx=clang++-19",
                             "--ld=clang-19", "--disable-autodetect", "--disable-doc", "--disable-debug"]
                configs[project] = ("config.h", "config_components.h")
            else:
                configure = [str(tools["baseline"]), *common(project), "defconfig"]
                configs[project] = (".config",)
            execute(configure, template, root / (project + "-configure.log"), project + " configuration")
            unexpected = [name for name in objects(template)
                          if project != "linux" or not name.startswith("scripts/")]
            if unexpected:
                raise RuntimeError("Configuration compiled release objects: " + repr(unexpected[:10]))
        (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        for index, (project, jobs, scenarios) in enumerate(plan):
            source, template = sources[project], templates[project]
            artifacts = ("ffmpeg", "ffprobe") if project == "ffmpeg" else ("vmlinux", "arch/x86/boot/bzImage")
            changed_source = source / ("fftools/ffmpeg.c" if project == "ffmpeg" else "init/main.c")
            required = "fftools/ffmpeg.o" if project == "ffmpeg" else "init/main.o"
            unrelated = "libavutil/adler32.o" if project == "ffmpeg" else "kernel/fork.o"
            config_hashes = {name: digest(template / name) for name in configs[project]}
            expected = {}
            order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
            for variant in order:
                build = root / (project + "-j" + str(jobs) + "-" + variant)
                shutil.copytree(template, build, symlinks=True)
                for scenario in scenarios:
                    previous = objects(build)
                    timestamp = changed_source.stat()
                    try:
                        if scenario == "source":
                            os.utime(changed_source, None)
                        phase = execute([str(tools[variant]), "build", "-j" + str(jobs),
                                         *common(project), "all"], build, build / (scenario + ".log"),
                                        project + " j" + str(jobs) + " " + variant + " " + scenario)
                    finally:
                        if scenario == "source":
                            os.utime(changed_source, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
                    current = objects(build)
                    changed = sorted(name for name in previous.keys() | current.keys()
                                     if previous.get(name) != current.get(name))
                    if scenario == "noop" and changed and variant == "candidate":
                        raise RuntimeError("Candidate no-op rebuilt objects: " + repr(changed[:10]))
                    if scenario == "source" and (required not in changed or unrelated in changed):
                        raise RuntimeError("Incorrect single-source rebuild set: " + repr(changed[:10]))
                    output_hashes = {name: digest(build / name) for name in artifacts}
                    if expected.get(scenario, output_hashes) != output_hashes:
                        raise RuntimeError("Baseline/candidate artifact mismatch: " + project + "/" + scenario)
                    if scenario != "clean" and output_hashes != expected["clean"]:
                        raise RuntimeError("Incremental build changed deterministic release artifacts")
                    if release_artifacts.get(project, output_hashes) != output_hashes:
                        raise RuntimeError("Release artifacts differ across job counts")
                    release_artifacts[project] = output_hashes
                    if {name: digest(build / name) for name in configs[project]} != config_hashes:
                        raise RuntimeError("Build configuration changed")
                    expected[scenario] = output_hashes
                    row = dict(project=project, entrypoint="build", jobs=jobs, sample=0, warmup=False,
                               variant=variant, scenario=scenario, total_seconds=phase["seconds"],
                               phases=dict(invocation=phase), artifacts=output_hashes,
                               changed_objects=changed, config_sha256=config_hashes)
                    rows.append(row)
                    with (root / "samples.jsonl").open("a") as output:
                        output.write(json.dumps(row) + "\n")
                if project == "ffmpeg":
                    check = run([str(build / "ffmpeg"), "-v", "error", "-f", "lavfi", "-i",
                                 "testsrc2=size=64x64:rate=10", "-t", "0.2", "-c:v", "mpeg4",
                                 "-f", "null", "-"], build, build / "smoke.log", env, journal)
                    smoke.append(check)
    except Exception as error:
        failures.append(str(error))
        raise
    finally:
        source_changes = {}
        for project, before in before_sources.items():
            after = source_manifest(sources[project])
            source_changes[project] = sorted(name for name in before.keys() | after.keys()
                                             if before.get(name) != after.get(name))
        summary = paired_summary(rows)
        for item in summary:
            item["bootstrap_95_percent"] = None
            item["inference"] = "Descriptive only; one pair cannot establish a confidence interval or acceptance gate."
        report = dict(metadata=metadata, commands_consumed=used,
                      commands_started_here=used - args.used_builds, failures=failures,
                      source_changes=source_changes, summary=summary, smoke=smoke,
                      original_statistical_gates_passed=False)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if any(source_changes.values()):
            raise RuntimeError("Release sources changed; see report")


if __name__ == "__main__":
    main()
