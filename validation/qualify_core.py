#!/usr/bin/env python3
# Copyright 2026 Openmktr contributors
# SPDX-License-Identifier: MPL-2.0
"""PC-only paired release qualification; retains evidence and never patches sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from benchmark_ninja import memory_summary, parity_summary

HERE = Path(__file__).resolve().parent
SCENARIOS = ("clean", "noop", "source", "missing", "graph")


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def make_plan(config, samples, warmups):
    root = Path(config["root"]).resolve()
    candidate = str(Path(config["candidate"]).resolve())
    baseline = str(Path(config["make"]).resolve())
    archives = Path(config["archives"]).resolve()
    campaign = json.loads((HERE / "dependency-campaign.json").read_text())
    releases = {item["id"]: item for item in campaign["components"]}
    jobs = config.get("jobs", [8, 16])
    if not jobs or any(not isinstance(job, int) or job < 1 for job in jobs):
        raise ValueError("jobs must be a nonempty list of positive integers")
    entries = config.get("entrypoints", ["direct", "build"])
    if set(entries) != {"direct", "build"} or len(entries) != 2:
        raise ValueError("qualification requires direct and build entrypoints")
    plan = []
    for project in ("linux", "ffmpeg"):
        output = root / project
        command = [sys.executable, str(HERE / "benchmark_ninja.py"), "--root", str(output),
                   "--archives", str(archives), "--project", project,
                   "--release-version", releases[project]["version"],
                   "--baseline", baseline, "--baseline-kind", "make",
                   "--baseline-revision", config["make_revision"],
                   "--candidate", candidate, "--candidate-revision", config["candidate_revision"],
                   "--samples", str(samples), "--warmups", str(warmups),
                   "--entrypoints", *entries, "--jobs", *map(str, jobs)]
        for option in ("toolchain_dir", "ninja_dir", "llvm_version"):
            if config.get(option) is not None:
                command += ["--" + option.replace("_", "-"), str(config[option])]
        plan.append(dict(project=project, report=str(output / "report.json"), command=command))
    lo = config["libreoffice"]
    for entrypoint in entries:
        for job in jobs:
            for sample in range(samples + warmups):
                # The exact pair order reverses each time, including warmups.
                order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
                for variant in order:
                    lane = "gnu" if variant == "baseline" else "direct" if entrypoint == "direct" else "safe"
                    work = root / "libreoffice" / entrypoint / str(job) / str(sample) / variant
                    command = [sys.executable, str(HERE / "benchmark_libreoffice.py"),
                               "--workspace", str(work), "--report", str(work / "evidence"),
                               "--omktr", candidate, "--archive", str(archives / lo["archive"]),
                               "--release-version", lo["version"], "--configuration", lo["configuration"],
                               "--make", baseline, "--cc", config["cc"], "--cxx", config["cxx"],
                               "--jobs", str(job), "--lanes", lane, "--repeats", "1", "--incremental"]
                    if config.get("ninja_dir"):
                        command += ["--ninja-dir", config["ninja_dir"]]
                    for option in ("source_probe", "output_probe", "unrelated_probe"):
                        if lo.get(option):
                            command += ["--" + option.replace("_", "-"), lo[option]]
                    plan.append(dict(project="libreoffice", entrypoint=entrypoint, jobs=job,
                                     sample=sample, warmup=sample < warmups, variant=variant,
                                     lane=lane, workspace=str(work), report=str(work / "report.json"), command=command))
    return plan


def libreoffice_rows(item, report):
    lane = item["lane"]
    data = report["lanes"][lane]
    if report["status"] != "builds-and-incremental-checks-finished":
        raise ValueError("incomplete LibreOffice lane")
    if len(data.get("incremental_checks", [])) != 3 or not all(check["passed"] for check in data["incremental_checks"]):
        raise ValueError("missing or failed LibreOffice incremental checks")
    if len(data.get("repeat_checks", [])) != 1 or data["repeat_checks"][0]["blocking_changes"]:
        raise ValueError("missing or failed LibreOffice no-op check")
    rows = []
    for scenario in SCENARIOS:
        suffix = "build" if scenario == "clean" else "noop-1" if scenario == "noop" else "incremental-" + scenario
        run = report["runs"][lane + "-" + suffix]
        if run["exit_code"]:
            raise ValueError("failed LibreOffice command: " + suffix)
        rows.append({**{key: item[key] for key in ("project", "entrypoint", "jobs", "sample", "warmup", "variant")},
                     "scenario": scenario, "total_seconds": run["wall_seconds"],
                     "peak_rss_kib": run["peak_individual_rss_kib"],
                     "sampled_peak_tree_rss_bytes": run["sampled_peak_tree_rss_bytes"],
                     "evidence": item["report"]})
    return rows


def compare_libreoffice(a, b):
    """Compare configuration and semantic products; retain binary hashes separately."""
    if a["normalized_config_sha256"] != b["normalized_config_sha256"]:
        raise ValueError("LibreOffice configurations differ between tools")
    def inventory(lane):
        return {name: ("symlink", record["symlink"]) if "symlink" in record else ("file",)
                for name, record in lane["installed_manifest"].items()}
    if inventory(a) != inventory(b):
        raise ValueError("LibreOffice installed artifact inventories differ")
    if a.get("smoke_texts") != b.get("smoke_texts") or set(a.get("smoke_texts", {})) != {"writer", "calc", "impress"}:
        raise ValueError("LibreOffice application output differs between tools")
    return dict(configuration_equal=True, inventory_equal=True, application_output_equal=True,
                binary_hashes_equal=a["installed_manifest"] == b["installed_manifest"],
                binary_hash_policy="retained; build paths and build IDs are not normalized")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--retain-builds", action="store_true")
    args = parser.parse_args()
    if args.samples < 7 or args.warmups < 2:
        parser.error("qualification requires at least seven measured pairs and two warmups")
    config = json.loads(args.config.read_text())
    plan = make_plan(config, args.samples, args.warmups)
    if args.plan_only:
        print(json.dumps(plan, indent=2))
        return
    root = Path(config["root"]).resolve()
    root.mkdir(parents=True, exist_ok=False)
    for field in ("candidate", "make"):
        if not Path(config[field]).is_file():
            parser.error("missing executable: " + config[field])
    make_version = subprocess.check_output([config["make"], "--version"], text=True)
    if "GNU Make" not in make_version:
        parser.error("the external comparison executable must be GNU Make")
    report = dict(schema=1, status="running", config=config, samples=args.samples, warmups=args.warmups,
                  candidate_sha256=sha(config["candidate"]), make_sha256=sha(config["make"]),
                  make_version=make_version, plan=plan, completed=[], libreoffice_rows=[],
                  libreoffice_comparisons=[], release_qualified=False)
    def save():
        temporary = root / "qualification.tmp"
        temporary.write_text(json.dumps(report, indent=2) + "\n")
        temporary.replace(root / "qualification.json")
    save()
    pairs = {}
    try:
        for index, item in enumerate(plan):
            with (root / ("phase-%03d.log" % index)).open("w") as log:
                result = subprocess.run(item["command"], stdout=log, stderr=subprocess.STDOUT)
            report["completed"].append(dict(index=index, status=result.returncode, report=item["report"]))
            save()
            if result.returncode:
                raise RuntimeError("qualification phase failed: " + str(index))
            evidence = json.loads(Path(item["report"]).read_text())
            if item["project"] == "libreoffice":
                report["libreoffice_rows"].extend(libreoffice_rows(item, evidence))
                key = (item["entrypoint"], item["jobs"], item["sample"])
                pair = pairs.setdefault(key, {})
                pair[item["variant"]] = evidence["lanes"][item["lane"]]
                if set(pair) == {"baseline", "candidate"}:
                    report["libreoffice_comparisons"].append(
                        dict(entrypoint=key[0], jobs=key[1], sample=key[2],
                             **compare_libreoffice(pair["baseline"], pair["candidate"])))
                    del pairs[key]
                if not args.retain_builds:
                    # Only remove trees created by this exact, fresh phase.
                    # Logs, output manifests, application PDFs and text stay.
                    base = Path(item["workspace"]) / item["lane"]
                    for name in ("build", "libreoffice-" + config["libreoffice"]["version"]):
                        shutil.rmtree(base / name)
            save()
        report["libreoffice_timing"] = parity_summary(report["libreoffice_rows"])
        report["libreoffice_memory"] = memory_summary(report["libreoffice_rows"])
        expected = len(config.get("jobs", [8, 16])) * 2 * len(SCENARIOS)
        lo_pass = all(len(report[field]) == expected and
                      all(cell["pairs"] == args.samples and cell["parity_passed"] for cell in report[field])
                      for field in ("libreoffice_timing", "libreoffice_memory"))
        other_pass = all(json.loads(Path(item["report"]).read_text())["parity_passed"]
                         for item in plan if item["project"] != "libreoffice")
        report["release_qualified"] = lo_pass and other_pass and not pairs
        report["status"] = "passed" if report["release_qualified"] else "qualification-failed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        save()
    if not report["release_qualified"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
