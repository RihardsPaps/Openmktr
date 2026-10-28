#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Continue paired qualification using retained configurations and one ledger.

Never converts a selected subset into the complete twelve-scenario promotion
gate. One warmup and seven alternating measured pairs remain mandatory.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from benchmark_ninja import digest, objects, paired_summary, run, source_manifest
from qualify_dependencies import Budget, check_timestamp_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--baseline-profile", choices=("safe", "legacy"), default="safe")
    parser.add_argument("--baseline-revision")
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--ledger", type=Path, required=True)
    parser.add_argument("--command-limit", type=int, default=200,
                        help="explicit user-authorized total cap; never resets the ledger")
    parser.add_argument("--case", action="append", required=True,
                        help="PROJECT:JOBS:clean,noop,source (repeatable)")
    parser.add_argument("--direct", choices=("linux", "ffmpeg"))
    args = parser.parse_args()
    cases = []
    for value in args.case:
        project, jobs, scenarios = value.split(":")
        scenarios = scenarios.split(",")
        if (project not in ("linux", "ffmpeg") or int(jobs) not in (8, 16) or
                scenarios[0] != "clean" or len(set(scenarios)) != len(scenarios) or
                any(s not in ("clean", "noop", "source") for s in scenarios)):
            parser.error("cases require linux/ffmpeg, j8/j16 and clean first")
        cases.append((project, int(jobs), scenarios))
    if len({(p, j) for p, j, _ in cases}) != len(cases):
        parser.error("each project/job combination must appear once")
    if args.direct and args.direct not in {p for p, _, _ in cases}:
        parser.error("direct checks require a paired case for that project")
    budget = Budget(args.ledger.resolve(), limit=args.command_limit)
    needed = sum(16 * len(s) for _, _, s in cases) + (2 if args.direct else 0)
    if budget.used + needed > budget.limit:
        parser.error(f"need {needed} slots; only {budget.limit-budget.used} remain; no reset")
    old = args.campaign.resolve()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    tools = dict(baseline=args.baseline.resolve(), candidate=args.candidate.resolve())
    env = json.loads((old / "metadata.json").read_text())["environment"]
    env.update(PATH=os.environ["PATH"], KBUILD_BUILD_TIMESTAMP="2024-11-18 00:00:00")
    check_timestamp_environment(env)
    releases = json.loads((old / "metadata.json").read_text())["releases"]
    projects = {p for p, _, _ in cases} | ({args.direct} if args.direct else set())
    sources = {p: old / "sources" / releases[p]["directory"] for p in projects}
    snapshots = {p: source_manifest(s) for p, s in sources.items()}
    metadata = dict(protocol="one discarded warmup pair; seven alternating measured pairs",
                    cases=cases, baseline_profile=args.baseline_profile, candidate_profile="aggressive",
                    baseline_revision=args.baseline_revision,
                    binaries={v: digest(t) for v, t in tools.items()}, environment=env,
                    harness_sha256=digest(Path(__file__)), prior_campaign=str(old),
                    image=os.environ.get("OMKTR_BENCHMARK_IMAGE_ID"),
                    affinity=sorted(os.sched_getaffinity(0)),
                    packages=subprocess.check_output(["apk", "info", "-v"], text=True).splitlines(),
                    releases=releases, previously_used=budget.used, planned_commands=needed,
                    command_limit=budget.limit)
    metadata["comparison_kind"] = ("profile isolation" if metadata["binaries"]["baseline"] ==
                                   metadata["binaries"]["candidate"] else "candidate regression comparison")
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows, direct, failures, expected = [], [], [], {}
    config_names = dict(linux=(".config",), ffmpeg=("config.h", "config_components.h"))
    artifacts = dict(linux=("vmlinux", "arch/x86/boot/bzImage"), ffmpeg=("ffmpeg", "ffprobe"))

    def common(project):
        return (["-f", str(sources[project] / "Makefile"), "LLVM=1", "LLVM_IAS=1",
                 "ARCH=x86", "HOSTCC=clang", "HOSTCXX=clang++"] if project == "linux" else [])

    def execute(command, build, purpose):
        reservation = budget.reserve(command, build, purpose)
        print(json.dumps(dict(event="start", **reservation)), flush=True)
        result = run(command, build, root / f"command-{budget.used:03d}.log", env,
                     root / "commands.jsonl")
        print(json.dumps(dict(event="finish", slot=budget.used, **result)), flush=True)
        return result

    def verify_artifacts(project, build):
        if project == "linux":
            with (build / "vmlinux").open("rb") as stream:
                header = stream.read(64)
            if (header[:6] != b"\x7fELF\x02\x01" or
                    int.from_bytes(header[18:20], "little") != 62):
                raise RuntimeError("vmlinux is not an ELF64 x86-64 artifact")
            with (build / "arch/x86/boot/bzImage").open("rb") as stream:
                stream.seek(0x202)
                if stream.read(4) != b"HdrS":
                    raise RuntimeError("bzImage has no x86 boot header")
        return {n: digest(build / n) for n in artifacts[project]}

    def report(status, source_changes=None):
        summary = paired_summary(rows)
        selected = (len(summary) == sum(len(s) for _, _, s in cases) and
                    all(s["supported_improvement"] for s in summary) and not failures)
        result = dict(status=status, metadata=metadata, summary=summary, direct=direct,
                      failures=failures, source_changes=source_changes,
                      consumed_commands=budget.used, selected_performance_gate_passed=selected,
                      full_promotion_performance_gate_passed=(status == "completed" and selected and
                                                            len(summary) == 12 and
                                                            not any((source_changes or {}).values())),
                      default_profile="safe; complete promotion and correctness gates required")
        temporary = root / "report.tmp"
        temporary.write_text(json.dumps(result, indent=2) + "\n")
        temporary.replace(root / "report.json")

    try:
        for project, jobs, scenarios in cases:
            source = sources[project]
            template = old / (project + "-configuration")
            config = {n: digest(template / n) for n in config_names[project]}
            changed_source = source / ("init/main.c" if project == "linux" else "fftools/ffmpeg.c")
            required = "init/main.o" if project == "linux" else "fftools/ffmpeg.o"
            unrelated = "kernel/fork.o" if project == "linux" else "libavutil/adler32.o"
            for sample in range(8):
                order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
                for variant in order:
                    build = root / f"{project}-j{jobs}-s{sample}-{variant}"
                    shutil.copytree(template, build, symlinks=True)
                    for scenario in scenarios:
                        before = objects(build)
                        stamp = changed_source.stat()
                        try:
                            if scenario == "source":
                                os.utime(changed_source, None)
                            profile = (["--graph_optimizations=aggressive"] if variant == "candidate" else
                                       ["--graph_optimizations=safe"] if args.baseline_profile == "safe" else [])
                            phase = execute([str(tools[variant]), "build", f"-j{jobs}", *profile,
                                             *common(project), "all"], build,
                                            f"followup {project} j{jobs} s{sample} {variant} {scenario}")
                        finally:
                            if scenario == "source":
                                os.utime(changed_source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                        after = objects(build)
                        changed = sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))
                        if scenario == "noop" and changed:
                            raise RuntimeError("No-op rebuilt objects: " + repr(changed[:10]))
                        if scenario == "source" and (required not in changed or unrelated in changed):
                            raise RuntimeError("Incorrect source rebuild set")
                        hashes = verify_artifacts(project, build)
                        if hashes != expected.setdefault(project, hashes):
                            raise RuntimeError("Artifacts differ across tools, jobs or rebuilds")
                        if config != {n: digest(build / n) for n in config}:
                            raise RuntimeError("Shared configuration changed")
                        row = dict(project=project, entrypoint="build", jobs=jobs, sample=sample,
                                   warmup=sample == 0, variant=variant, scenario=scenario,
                                   total_seconds=phase["seconds"], phases=dict(invocation=phase),
                                   artifacts=hashes, config_sha256=config, changed_objects=changed)
                        rows.append(row)
                        with (root / "samples.jsonl").open("a") as output:
                            output.write(json.dumps(row) + "\n")
                    if project == "ffmpeg":
                        run([str(build / "ffmpeg"), "-v", "error", "-f", "lavfi", "-i",
                             "testsrc2=size=64x64:rate=10", "-t", "0.2", "-c:v", "mpeg4",
                             "-f", "null", "-"], build, root / f"smoke-j{jobs}-s{sample}-{variant}.log",
                            env, root / "commands.jsonl")
                    shutil.rmtree(build)
                report("running")
        if args.direct:
            project = args.direct
            build = root / (project + "-direct")
            shutil.copytree(old / (project + "-configuration"), build, symlinks=True)
            config = {n: digest(build / n) for n in config_names[project]}
            for scenario in ("clean", "noop"):
                before = objects(build)
                phase = execute([str(tools["candidate"]), "-j8", *common(project), "all"],
                                build, f"followup {project} direct {scenario}")
                hashes = verify_artifacts(project, build)
                if hashes != expected[project] or (scenario == "noop" and objects(build) != before):
                    raise RuntimeError("Direct artifacts/repeat differ")
                if config != {n: digest(build / n) for n in config}:
                    raise RuntimeError("Direct shared configuration changed")
                direct.append(dict(project=project, scenario=scenario, artifacts=hashes,
                                   config_sha256=config, phase=phase))
            shutil.rmtree(build)
    except BaseException as error:
        failures.append(str(error))
        raise
    finally:
        changes = {p: source_manifest(sources[p]) != s for p, s in snapshots.items()}
        report("failed" if failures or any(changes.values()) else "completed", changes)
        budget.close()
        if any(changes.values()):
            raise RuntimeError("Release source contents changed")


if __name__ == "__main__":
    main()
