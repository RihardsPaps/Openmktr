#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""External differential incremental campaign (GNU Make is an oracle only).

Run on POSIX with Python, Clang, GNU Make, Ninja and a built Openmktr.
All source mutations and graph generation happen in retained disposable copies.
See incremental/README.md for the manifest contract and release policy.
"""

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def contained(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes disposable tree: {name}")
    return path


def inventory(root):
    return {p.relative_to(root).as_posix(): digest(p)
            for p in sorted(root.rglob("*")) if p.is_file()}


def snapshot(root, patterns):
    result = {}
    for pattern in patterns:
        for name in glob.glob(str(root / pattern), recursive=True):
            path = contained(root, os.path.relpath(name, root))
            if path.is_file():
                stat = path.stat()
                result[path.relative_to(root).as_posix()] = {
                    "sha256": digest(path), "mtime_ns": stat.st_mtime_ns,
                    "size": stat.st_size, "inode": stat.st_ino,
                }
    return result


def rebuilt(before, after):
    return {name for name in before.keys() | after.keys()
            if before.get(name) != after.get(name)}


def classify(reference, candidate, expected=None):
    """Keep correctness failures separate from performance defects."""
    required = set(reference) | set(expected or ())
    allowed = set(reference) if expected is None else set(expected)
    return {"missed_rebuilds": sorted(required - set(candidate)),
            "unnecessary_rebuilds": sorted(set(candidate) - allowed)}


def mutate(root, changes):
    # Advance only edited inputs beyond every existing timestamp, then wait
    # until wall time catches up. No future-dated input or coarse-FS race.
    if not changes:
        return
    latest = max((p.stat().st_mtime_ns for p in root.rglob("*")
                  if p.is_file()), default=time.time_ns())
    stamp = max(time.time_ns(), latest) + 1_100_000_000
    for change in changes:
        path = contained(root, change["path"])
        action = change["action"]
        if action == "delete":
            path.unlink()
            continue
        if action == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(change["text"], encoding="utf-8")
        elif action == "replace":
            text = path.read_text(encoding="utf-8")
            if text.count(change["old"]) != 1:
                raise ValueError(f"Replacement must match exactly once: {path}")
            path.write_text(text.replace(change["old"], change["new"]),
                            encoding="utf-8")
        else:
            raise ValueError(f"Unknown mutation: {action}")
        os.utime(path, ns=(stamp, stamp))
    while time.time_ns() <= stamp:
        time.sleep(0.02)


class Campaign:
    def __init__(self, args):
        self.args = args
        self.root = args.output.resolve()
        self.root.mkdir(parents=True, exist_ok=False)
        self.commands = []
        # Do not inherit MAKEFLAGS, compiler caches, or a parent jobserver.
        self.env = {key: os.environ[key] for key in ("PATH", "HOME", "TMPDIR")
                    if key in os.environ}
        self.env.update(LC_ALL="C", TZ="UTC", CCACHE_DISABLE="1",
                        SOURCE_DATE_EPOCH="1700000000")
        for assignment in args.env:
            key, value = assignment.split("=", 1)
            self.env[key] = value
        self.tools = dict(make=args.make, omktr=str(args.omktr.resolve()),
                          cc=args.cc, python=sys.executable, ninja="ninja")

    def expand(self, args):
        return [arg.format(**self.tools) for arg in args]

    def run(self, tree, command, label, interrupt=None):
        log = tree.parent / f"{tree.name}-{label}.log"
        command = self.expand(command)
        entry = dict(cwd=str(tree), command=command, log=str(log))
        self.commands.append(entry)
        start = time.monotonic()
        with log.open("wb") as stream:
            child = subprocess.Popen(command, cwd=tree, env=self.env,
                                     stdout=stream, stderr=subprocess.STDOUT,
                                     start_new_session=True)
            try:
                if interrupt:
                    marker = contained(tree, interrupt)
                    while not marker.exists():
                        if child.poll() is not None:
                            raise RuntimeError("Build finished before interrupt marker")
                        if time.monotonic() - start > self.args.timeout:
                            raise RuntimeError("Interrupt marker timed out")
                        time.sleep(0.02)
                    entry["interrupted_at"] = interrupt
                    os.killpg(child.pid, signal.SIGINT)
                status = child.wait(timeout=self.args.timeout)
            finally:
                # Reap the entire process group, including orphaned recipes.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
                entry.update(status=child.returncode,
                             seconds=time.monotonic() - start)
                (self.root / "commands.json").write_text(
                    json.dumps(self.commands, indent=2) + "\n")
        if interrupt and status == 0:
            raise RuntimeError("Interrupted build unexpectedly succeeded")
        if not interrupt and status:
            raise RuntimeError(f"Command failed ({status}): {log}")

    def build(self, tree, project, mode, label, interrupt=None):
        args = project.get("args", ["all"])
        options = ["-j" + str(self.args.jobs)] + args
        if mode != "make":
            options += getattr(self.args, "omktr_option", [])
        if mode == "ninja":
            self.run(tree, ["{omktr}", "--ninja", "--regen"] + options,
                     label + "-generate")
            self.run(tree, ["sh", "./ninja.sh", "-j" + str(self.args.jobs)],
                     label, interrupt)
        else:
            self.run(tree, ["{make}" if mode == "make" else "{omktr}"] + options,
                     label, interrupt)

    def dependencies(self, tree, project):
        # Clang -MMD depfiles: fixture paths deliberately contain no whitespace.
        # External manifests use this same restricted depfile contract.
        coverage = {}
        for name, required in project.get("dependencies", {}).items():
            path = contained(tree, name)
            words = (path.read_text().replace("\\\n", " ").split()
                     if path.exists() else [])
            coverage[name] = {"required": required,
                              "missing": sorted(set(required) - set(words)),
                              "observed": sorted(words)}
        return coverage

    def case(self, project, case, directory):
        directory.mkdir(parents=True)
        source = project["source"]
        trees = {}
        before = {}
        baseline_noop = {}
        results = {}
        artifacts = project["artifacts"]
        for mode in ("make", "direct", "ninja"):
            tree = directory / mode
            shutil.copytree(source, tree)
            trees[mode] = tree
            for index, command in enumerate(project.get("prepare", [])):
                self.run(tree, command, f"prepare-{index}")
            self.build(tree, project, mode, "baseline")
            if case.get("prime"):
                mutate(tree, case["prime"])
                self.build(tree, project, mode, "prime-dependencies")
            before[mode] = snapshot(tree, artifacts)
            if not before[mode]:
                raise RuntimeError("Artifact patterns matched no baseline outputs")
            self.build(tree, project, mode, "baseline-noop")
            baseline_noop[mode] = sorted(rebuilt(before[mode], snapshot(tree, artifacts)))

        # All cases start at an independent warm baseline; no cross-case state.
        for mode, tree in trees.items():
            mutate(tree, case.get("changes", []))
            recovery_before = snapshot(tree, artifacts)
            failure = None
            try:
                if "interrupt" in case:
                    control = case["interrupt"]
                    marker = contained(tree, control["marker"])
                    if marker.exists():
                        raise RuntimeError("Interrupt marker already exists")
                    contained(tree, control["arm"]).write_text("armed\n")
                    self.build(tree, project, mode, "interrupted", control["marker"])
                    contained(tree, control["arm"]).unlink()
                    # Count recovery work separately from the interrupted run.
                    recovery_before = snapshot(tree, artifacts)
                self.build(tree, project, mode, "incremental")
            except Exception as error:
                failure = str(error)
            after = snapshot(tree, artifacts)
            changed = rebuilt(recovery_before if "interrupt" in case else before[mode],
                              after)
            coverage = self.dependencies(tree, dict(project, dependencies=case.get(
                "dependencies", project.get("dependencies", {}))))
            results[mode] = {"rebuilt": sorted(changed), "before": before[mode],
                             "after": after, "dependencies": coverage,
                             "command_failure": failure,
                             "baseline_noop_rebuilt": baseline_noop[mode]}
            stable = snapshot(tree, artifacts)
            if not failure:
                self.build(tree, project, mode, "settled-noop")
            results[mode]["noop_rebuilt"] = sorted(rebuilt(
                stable, snapshot(tree, artifacts)))

        # A fresh GNU Make build from mutated pristine sources is independent
        # of all warm caches and detects stale artifacts shared by both engines.
        oracle = directory / "clean-oracle"
        shutil.copytree(source, oracle)
        for index, command in enumerate(project.get("prepare", [])):
            self.run(oracle, command, f"prepare-{index}")
        # Deleted build outputs do not exist in a pristine tree.
        mutate(oracle, case.get("prime", []))
        mutate(oracle, [change for change in case.get("changes", [])
                        if change["action"] != "delete"
                        or contained(oracle, change["path"]).exists()])
        self.build(oracle, project, "make", "clean")
        clean = snapshot(oracle, project.get("deterministic", artifacts))
        if not clean:
            raise RuntimeError("No deterministic oracle artifacts")
        reference = results["make"]["rebuilt"]
        for mode, result in results.items():
            result.update(classify(reference, result["rebuilt"], case.get("expected")))
            result["stale_or_missing_outputs"] = sorted(
                name for name, value in clean.items()
                if result["after"].get(name, {}).get("sha256") != value["sha256"])
            result["missing_dependencies"] = {
                name: item["missing"] for name, item in result["dependencies"].items()
                if item["missing"]}
            result["correctness_failure"] = bool(
                result["missed_rebuilds"] or result["stale_or_missing_outputs"]
                or result["missing_dependencies"] or result["command_failure"])
            result["performance_defect"] = bool(
                result["unnecessary_rebuilds"] or result["noop_rebuilt"])
            result["performance_defect"] |= bool(result["baseline_noop_rebuilt"])
        report = dict(project=project["name"], scenario=case["name"],
                      results=results, clean_oracle=clean)
        (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name(
        "incremental") / "projects.json")
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory; retains trees, logs and report")
    parser.add_argument("--omktr", type=Path, required=True)
    parser.add_argument("--omktr-option", action="append", default=[],
                        help="extra Openmktr option only; repeat with =--OPTION")
    parser.add_argument("--make", default="make")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="Explicit environment override, recorded in report")
    parser.add_argument("--project", action="append")
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    if os.name != "posix" or args.jobs < 1 or args.timeout <= 0:
        parser.error("Requires POSIX, positive --jobs and --timeout")
    if any("=" not in value or not value.split("=", 1)[0] for value in args.env):
        parser.error("--env requires NAME=VALUE")
    manifest = json.loads(args.manifest.read_text())
    projects = [dict(manifest.get("defaults", {}), **project)
                for project in manifest["projects"]]
    if args.project:
        projects = [p for p in projects if p["name"] in args.project]
    if not projects:
        parser.error("No selected projects")
    for project in projects:
        source = (args.manifest.resolve().parent / project["source"]).resolve()
        if not source.is_dir() or not any(source.iterdir()):
            parser.error(f"Missing or empty source tree: {source}")
        if args.output.resolve().is_relative_to(source):
            parser.error("Evidence directory must be outside pristine source trees")
    # Reject bad selections before writing a report that might look complete.
    known_cases = {c["name"] for p in projects for c in p["cases"]}
    if args.case and not set(args.case) <= known_cases:
        parser.error("Unknown scenario")
    campaign = Campaign(args)
    report = {"schema": 1, "projects": [], "runs": [], "errors": [],
              "environment": campaign.env,
              "manifest": {"path": str(args.manifest.resolve()),
                           "sha256": digest(args.manifest), "content": manifest},
              "selection": {"projects": args.project, "cases": args.case,
                            "jobs": args.jobs, "omktr_options": args.omktr_option}, "tools": {}}
    try:
        for name, tool in campaign.tools.items():
            path = Path(shutil.which(tool) or tool).resolve()
            version = subprocess.check_output([str(path), "--version"],
                                              env=campaign.env, text=True)
            report["tools"][name] = dict(path=str(path), sha256=digest(path),
                                           version=version)
        if not report["tools"]["make"]["version"].startswith("GNU Make "):
            raise RuntimeError("--make must be an external GNU Make oracle")
        for project in projects:
            source = (args.manifest.resolve().parent / project["source"]).resolve()
            project["source"] = source
            pristine = inventory(source)
            report["projects"].append(dict(name=project["name"], source=str(source),
                                            source_hashes=pristine))
            for case in project["cases"]:
                if args.case and case["name"] not in args.case:
                    continue
                print(f"{project['name']}: {case['name']}", flush=True)
                directory = contained(campaign.root,
                                      project["name"] + "/" + case["name"])
                try:
                    report["runs"].append(campaign.case(project, case, directory))
                except Exception as error:
                    report["errors"].append(dict(project=project["name"],
                                                 scenario=case["name"], error=str(error)))
                finally:
                    if inventory(source) != pristine:
                        raise RuntimeError(f"Original source changed: {source}")
    except Exception as error:
        report["errors"].append(dict(error=str(error)))
    correctness = any(r["correctness_failure"] for run in report["runs"]
                      for r in run["results"].values())
    performance = any(r["performance_defect"] for run in report["runs"]
                      for r in run["results"].values())
    report.update(release_blocked=bool(correctness or report["errors"]),
                  performance_defects=performance)
    (campaign.root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in (
        "release_blocked", "performance_defects", "errors")}, indent=2))
    return 2 if report["errors"] else 1 if correctness else 3 if performance else 0


if __name__ == "__main__":
    sys.exit(main())
