#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Check Linux recovery and fresh direct/Ninja artifacts after a failed campaign.

Consumes the original ledger. This is correctness qualification, not a substitute
for the incomplete statistical performance gate. Never edits release sources.
"""
import argparse
import json
import os
from pathlib import Path
import shutil

from benchmark_ninja import digest, objects, run, source_manifest
from qualify_dependencies import Budget, check_timestamp_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--budget-ledger", type=Path, required=True)
    args = parser.parse_args()
    old = args.campaign.resolve()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    budget = Budget(args.budget_ledger.resolve())
    metadata = json.loads((old / "metadata.json").read_text())
    env = metadata["environment"]
    env["PATH"] = os.environ["PATH"]
    # The campaign adapter supplies Kbuild's -d ISO conversion. BSD date did
    # not support -d and silently left initramfs mtimes unfixed.
    env["KBUILD_BUILD_TIMESTAMP"] = "2024-11-18 00:00:00"
    check_timestamp_environment(env)
    tool = args.tool.resolve()
    source = old / "sources" / metadata["releases"]["linux"]["directory"]
    before_source = source_manifest(source)
    common = ["-f", str(source / "Makefile"), "LLVM=1", "LLVM_IAS=1", "ARCH=x86",
              "HOSTCC=clang", "HOSTCXX=clang++"]
    rows, failures, defects = [], [], []
    expected = None

    def execute(command, cwd, purpose):
        reservation = budget.reserve(command, cwd, purpose)
        print(json.dumps(dict(event="start", **reservation)), flush=True)
        phase = run(command, cwd, root / f"command-{budget.used:03d}.log", env,
                    root / "commands.jsonl")
        print(json.dumps(dict(event="finish", slot=budget.used, **phase)), flush=True)
        return phase

    def artifacts(build):
        with (build / "vmlinux").open("rb") as stream:
            header = stream.read(20)
        with (build / "arch/x86/boot/bzImage").open("rb") as stream:
            stream.seek(0x202)
            boot = stream.read(4)
        if header[:4] != b"\x7fELF" or header[18:20] != b"\x3e\x00" or boot != b"HdrS":
            raise RuntimeError("Invalid Linux x86 artifacts")
        return {p: digest(build / p) for p in ("vmlinux", "arch/x86/boot/bzImage")}

    try:
        failed = old / "linux-j8-s0-baseline"
        phase = execute([str(tool), "build", "-j8", "--graph_optimizations=safe", *common, "all"],
                        failed, "linux recovery after external sed prerequisite fix")
        rows.append(dict(mode="recovery", scenario="recovery", phase=phase,
                         artifacts=artifacts(failed), reference_eligible=False))
        # The failed environment could not parse Kbuild's fixed initramfs date.
        # Establish the deterministic reference from the first fresh build in
        # the corrected environment, not from retained pre-fix initramfs bytes.
        template = old / "linux-configuration"
        expected_config = digest(template / ".config")
        modes = (("ninja-safe", ["build", "-j8", "--graph_optimizations=safe"]),
                 ("ninja-aggressive", ["build", "-j16", "--graph_optimizations=aggressive"]),
                 ("direct", ["-j8"]))
        for mode, options in modes:
            build = root / mode
            shutil.copytree(template, build, symlinks=True)
            for scenario in ("clean", "noop", "source"):
                before = objects(build)
                changed_source = source / "init/main.c"
                timestamp = changed_source.stat()
                try:
                    if scenario == "source":
                        os.utime(changed_source, None)
                    phase = execute([str(tool), *options, *common, "all"], build,
                                    "linux correctness " + mode + " " + scenario)
                finally:
                    if scenario == "source":
                        os.utime(changed_source, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
                after = objects(build)
                changed = sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))
                hashes = artifacts(build)
                if expected is None:
                    expected = hashes
                if hashes != expected or digest(build / ".config") != expected_config:
                    raise RuntimeError("Linux artifacts/configuration differ across modes or rebuilds")
                if scenario == "noop" and changed:
                    defects.append(dict(mode=mode, scenario=scenario, changed_objects=changed))
                if scenario == "source" and ("init/main.o" not in changed or "kernel/fork.o" in changed):
                    raise RuntimeError("Incorrect Linux single-source rebuild set")
                rows.append(dict(mode=mode, scenario=scenario, phase=phase,
                                 artifacts=hashes, changed_objects=changed))
            shutil.rmtree(build)
    except Exception as error:
        failures.append(str(error))
        raise
    finally:
        changed = source_manifest(source) != before_source
        report = dict(protocol="Linux recovery plus fresh safe/aggressive Ninja and direct correctness; no timing inference",
                      original_campaign=str(old), tool_sha256=digest(tool), environment=env,
                      image=os.environ.get("OMKTR_BENCHMARK_IMAGE_ID"),
                      harness_sha256=digest(Path(__file__)), rows=rows, failures=failures,
                      performance_defects=defects, source_changes=changed, consumed_commands=budget.used,
                      correctness_passed=len(rows) == 10 and not failures and not changed,
                      statistical_performance_qualification=False, default_promotion="retain safe")
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        budget.close()
        if changed:
            raise RuntimeError("Release source contents changed")


if __name__ == "__main__":
    main()
