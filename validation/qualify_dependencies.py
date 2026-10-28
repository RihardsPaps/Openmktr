#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Qualify current FFmpeg/Linux releases within one durable 200-command budget.

One shared configuration per release; one warmup and seven measured pairs.
Never retries failed commands. Use the same ledger for every attempt/diagnostic.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import time

from benchmark_ninja import digest, objects, paired_summary, run, source_manifest


class Budget:
    def __init__(self, path, limit=200):
        if limit < 1:
            raise ValueError("command limit must be positive")
        self.limit = limit
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = path.open("a+")
        try:
            fcntl.flock(self.stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.stream.seek(0)
            records = [json.loads(line) for line in self.stream]
            if [r["slot"] for r in records] != list(range(1, len(records) + 1)):
                raise RuntimeError("invalid budget ledger; refusing to reset consumed commands")
        except BaseException:
            self.stream.close()
            raise
        self.used = len(records)

    def reserve(self, command, cwd, purpose):
        if self.used >= self.limit:
            raise RuntimeError(f"{self.limit}-command budget exhausted; no process started")
        self.used += 1
        record = dict(slot=self.used, command=list(map(str, command)), cwd=str(cwd),
                      purpose=purpose, reserved_unix=time.time())
        self.stream.write(json.dumps(record) + "\n")
        self.stream.flush()
        os.fsync(self.stream.fileno())
        return record

    def close(self):
        self.stream.close()


def check_timestamp_environment(env):
    value = subprocess.check_output(
        ["date", "-d" + env["KBUILD_BUILD_TIMESTAMP"], "+%s"], env=env, text=True).strip()
    if value != env["SOURCE_DATE_EPOCH"]:
        raise RuntimeError("Campaign date conversion does not match SOURCE_DATE_EPOCH")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--budget-ledger", type=Path, required=True)
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("dependency-campaign.json"))
    args = parser.parse_args()
    budget = Budget(args.budget_ledger.resolve())
    if budget.used + 198 > 200:
        parser.error("qualification requires 198 remaining command slots; ledger is never reset")
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    tool = args.tool.resolve()
    env = dict(HOME="/root", USER="benchmark", LOGNAME="benchmark", SHELL="/bin/sh",
               PATH=os.environ["PATH"], CCACHE_DISABLE="1", LC_ALL="C", TZ="UTC",
               SOURCE_DATE_EPOCH="1731888000", KBUILD_BUILD_USER="benchmark",
               KBUILD_BUILD_HOST="benchmark", KBUILD_BUILD_VERSION="1",
               KBUILD_BUILD_TIMESTAMP="2024-11-18 00:00:00", M4="/usr/bin/gm4")
    check_timestamp_environment(env)
    rows, direct, failures, snapshots, sources, templates, config_names = [], [], [], {}, {}, {}, {}
    manifest = json.loads(args.manifest.read_text())
    releases = {p["id"]: p for p in manifest["components"] if p["id"] in ("ffmpeg", "linux")}
    metadata = dict(protocol="one discarded warmup pair; seven alternating measured pairs",
                    limit=200, previously_used=budget.used, tool_sha256=digest(tool),
                    harness_sha256=digest(Path(__file__)), manifest_sha256=digest(args.manifest),
                    baseline_profile="safe", candidate_profile="aggressive",
                    image=os.environ.get("OMKTR_BENCHMARK_IMAGE_ID"), environment=env,
                    affinity=sorted(os.sched_getaffinity(0)), platform=list(os.uname()),
                    versions={name: subprocess.check_output([name, "--version"], text=True)
                              for name in ("clang", "ninja", "python3")}, releases=releases)
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    def execute(command, cwd, purpose):
        reservation = budget.reserve(command, cwd, purpose)
        print(json.dumps(dict(event="start", **reservation)), flush=True)
        log = root / (f"command-{reservation['slot']:03d}.log")
        phase = run(command, cwd, log, env, root / "commands.jsonl")
        print(json.dumps(dict(event="finish", slot=reservation["slot"], **phase)), flush=True)
        return phase

    def common(project):
        return (["-f", str(sources[project] / "Makefile"), "LLVM=1", "LLVM_IAS=1", "ARCH=x86",
                 "HOSTCC=clang", "HOSTCXX=clang++"] if project == "linux" else [])

    try:
        for project in ("ffmpeg", "linux"):
            release = releases[project]
            archive = args.archives.resolve() / release["archive"]
            if digest(archive) != release["sha256"]:
                raise RuntimeError("Release archive checksum mismatch: " + project)
            with tarfile.open(archive) as incoming:
                incoming.extractall(root / "sources", filter="data")
            source = sources[project] = root / "sources" / release["directory"]
            snapshots[project] = source_manifest(source)
            template = templates[project] = root / (project + "-configuration")
            template.mkdir()
            if project == "ffmpeg":
                configure = [str(source / "configure"), "--cc=clang", "--cxx=clang++",
                             "--ld=clang", "--disable-autodetect", "--disable-doc", "--disable-debug"]
                config_names[project] = ("config.h", "config_components.h")
            else:
                configure = [str(tool), "--graph_optimizations=safe", *common(project), "defconfig"]
                config_names[project] = (".config",)
            execute(configure, template, project + " shared configuration")
        for project in ("ffmpeg", "linux"):
            source, template = sources[project], templates[project]
            artifacts = ("ffmpeg", "ffprobe") if project == "ffmpeg" else ("vmlinux", "arch/x86/boot/bzImage")
            changed_source = source / ("fftools/ffmpeg.c" if project == "ffmpeg" else "init/main.c")
            required = "fftools/ffmpeg.o" if project == "ffmpeg" else "init/main.o"
            unrelated = "libavutil/adler32.o" if project == "ffmpeg" else "kernel/fork.o"
            expected_config = {p: digest(template / p) for p in config_names[project]}
            for jobs in (8, 16):
                for sample in range(8):
                    expected = {}
                    order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
                    for variant in order:
                        profile = "safe" if variant == "baseline" else "aggressive"
                        build = root / f"{project}-j{jobs}-s{sample}-{variant}"
                        shutil.copytree(template, build, symlinks=True)
                        for scenario in ("clean", "noop", "source"):
                            before = objects(build)
                            timestamp = changed_source.stat()
                            try:
                                if scenario == "source":
                                    os.utime(changed_source, None)
                                phase = execute([str(tool), "build", "-j" + str(jobs),
                                                 "--graph_optimizations=" + profile,
                                                 *common(project), "all"], build,
                                                f"{project} j{jobs} s{sample} {variant} {scenario}")
                            finally:
                                if scenario == "source":
                                    os.utime(changed_source, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
                            after = objects(build)
                            changed = sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))
                            if scenario == "noop" and changed:
                                raise RuntimeError("No-op rebuilt objects: " + repr(changed[:10]))
                            if scenario == "source" and (required not in changed or unrelated in changed):
                                raise RuntimeError("Incorrect source rebuild set: " + repr(changed[:10]))
                            hashes = {p: digest(build / p) for p in artifacts}
                            if expected.get(scenario, hashes) != hashes or (scenario != "clean" and expected["clean"] != hashes):
                                raise RuntimeError("Deterministic release artifact mismatch")
                            if {p: digest(build / p) for p in config_names[project]} != expected_config:
                                raise RuntimeError("Build changed shared configuration")
                            expected[scenario] = hashes
                            row = dict(project=project, entrypoint="build", jobs=jobs, sample=sample,
                                       warmup=sample == 0, variant=variant, scenario=scenario,
                                       total_seconds=phase["seconds"], phases=dict(invocation=phase),
                                       artifacts=hashes, changed_objects=changed, config_sha256=expected_config)
                            rows.append(row)
                            with (root / "samples.jsonl").open("a") as output:
                                output.write(json.dumps(row) + "\n")
                        if project == "ffmpeg":
                            run([str(build / "ffmpeg"), "-v", "error", "-f", "lavfi", "-i",
                                 "testsrc2=size=64x64:rate=10", "-t", "0.2", "-c:v", "mpeg4",
                                 "-f", "null", "-"], build, build / "smoke.log", env, root / "commands.jsonl")
                        # Keep logs and hashes; release outputs are disposable and large.
                        shutil.rmtree(build)
            build = root / (project + "-direct")
            shutil.copytree(template, build, symlinks=True)
            for scenario in ("clean", "noop"):
                before = objects(build)
                phase = execute([str(tool), "-j8", *common(project), "all"], build,
                                project + " direct " + scenario)
                hashes = {p: digest(build / p) for p in artifacts}
                if hashes != expected["clean"] or (scenario == "noop" and objects(build) != before):
                    raise RuntimeError("Direct mode artifact/repeat mismatch")
                direct.append(dict(project=project, scenario=scenario, artifacts=hashes, phase=phase))
            shutil.rmtree(build)
    except Exception as error:
        failures.append(str(error))
        raise
    finally:
        changes = {p: source_manifest(sources[p]) != original for p, original in snapshots.items()}
        summary = paired_summary(rows)
        passed = (len(summary) == 12 and all(p["supported_improvement"] for p in summary)
                  and len(direct) == 4 and not failures and not any(changes.values()))
        report = dict(metadata=metadata, consumed_commands=budget.used, failures=failures,
                      source_changes=changes, summary=summary, direct=direct,
                      performance_gate_passed=passed,
                      default_promotion="eligible only after core correctness gates" if passed else "retain safe")
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        budget.close()
        if any(changes.values()):
            raise RuntimeError("Release sources changed; see report")


if __name__ == "__main__":
    main()
