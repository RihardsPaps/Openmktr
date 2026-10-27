#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Portable checks for paired Ninja performance evidence accounting."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "benchmark_ninja", Path(__file__).resolve().parents[1] / "validation/benchmark_ninja.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class Evidence(unittest.TestCase):
    def test_budgeted_campaign_rejects_extra_work_before_running_tools(self):
        script = Path(benchmark.__file__).with_name("benchmark_graph_budget.py")
        for limit, used in ((20, 3), (21, 0), (20, -1)):
            with self.subTest(limit=limit, used=used), tempfile.TemporaryDirectory() as temp:
                root = Path(temp) / "must-not-be-created"
                result = subprocess.run([
                    sys.executable, str(script), "--root", str(root),
                    "--archives", temp, "--baseline", str(Path(temp) / "missing-baseline"),
                    "--candidate", str(Path(temp) / "missing-candidate"),
                    "--toolchain-dir", temp, "--limit", str(limit), "--used-builds", str(used),
                ], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(root.exists())

    def test_effective_settings_record_defaults_and_ablations(self):
        self.assertFalse(benchmark.effective_settings([])["enabled"]["monitor"])
        aggressive = benchmark.effective_settings([
            "--graph_optimizations=aggressive", "--disable_graph_optimization=evaluation"])
        self.assertTrue(aggressive["enabled"]["recursive"])
        self.assertFalse(aggressive["general_state_replay"])
        self.assertFalse(any(benchmark.effective_settings(
            ["--graph_optimizations=off"])["enabled"].values()))

    def test_pairs_match_samples_and_discard_warmups_and_incomplete_pairs(self):
        def row(sample, variant, seconds, warmup=False):
            return dict(project="synthetic", entrypoint="build", jobs=8,
                        scenario="clean", sample=sample, variant=variant,
                        total_seconds=seconds, warmup=warmup)
        rows = [row(0, "baseline", 10, True), row(0, "candidate", 100, True),
                row(1, "baseline", 10), row(2, "candidate", 3),
                row(2, "baseline", 4), row(1, "candidate", 5),
                row(3, "baseline", 99)]
        report, = benchmark.paired_summary(rows)
        self.assertEqual(report["pairs"], 2)
        self.assertEqual(sorted(report["paired_improvement_percent"]), [25, 50])
        self.assertEqual(report["median_improvement_percent"], 37.5)
        self.assertEqual(report["bootstrap_95_percent"], [25, 50])
        self.assertFalse(report["supported_improvement"])

    def test_source_manifest_tracks_symlinks_and_removed_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "file").write_text("original")
            (root / "link").symlink_to("file")
            before = benchmark.source_manifest(root)
            (root / "file").unlink()
            (root / "link").unlink()
            (root / "link").symlink_to("missing")
            after = benchmark.source_manifest(root)
            self.assertEqual(before["link"], "link:file")
            self.assertEqual(after, {"link": "link:missing"})

    def test_failed_commands_remain_in_journal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            journal = root / "commands.jsonl"
            with self.assertRaises(RuntimeError):
                benchmark.run([sys.executable, "-c", "raise SystemExit(7)"], root,
                              root / "failure.log", dict(os.environ), journal)
            record = json.loads(journal.read_text())
            self.assertEqual(record["status"], 7)
            self.assertGreater(record["seconds"], 0)

    def test_retained_campaign_separates_setup_and_preserves_each_log(self):
        binary = Path(os.environ.get("OMKTR_BINARY", str(
            Path(__file__).resolve().parents[1] / "omktr"))).resolve()
        compilers = sorted(p for p in Path("/usr/bin").glob("clang-*")
                           if p.name.removeprefix("clang-").isdigit())
        if not binary.is_file() or not compilers:
            self.skipTest("built Openmktr and versioned Clang are required")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "campaign"
            result = subprocess.run([
                sys.executable, str(Path(benchmark.__file__)),
                "--root", str(root), "--project", "synthetic",
                "--baseline", str(binary), "--candidate", str(binary),
                "--baseline-revision", "test", "--candidate-revision", "test",
                "--baseline-option=--graph_optimizations=off",
                "--candidate-option=--graph_optimizations=aggressive",
                "--entrypoints", "build", "--jobs", "2", "--samples", "1",
                "--warmups", "1", "--llvm-version",
                compilers[-1].name.removeprefix("clang-"), "--retained", "--monitor",
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / "report.json").read_text())
            self.assertIsNone(report["failure"])
            self.assertEqual(report["metadata"]["build_state"], "retained incremental only")
            self.assertEqual({r["scenario"] for r in report["summary"]},
                             {"noop", "source", "missing", "graph"})
            self.assertTrue(all(r["pairs"] == 1 and not r["supported_improvement"]
                                for r in report["summary"]))
            rows = [json.loads(line) for line in (root / "samples.jsonl").read_text().splitlines()]
            self.assertEqual(len(rows), 16)
            self.assertEqual(sum(row["warmup"] for row in rows), 8)
            self.assertTrue(all(not row["changed_objects"] for row in rows
                                if row["scenario"] == "noop"))
            commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
            logs = [r["log"] for r in commands]
            self.assertEqual(len(logs), len(set(logs)))
            self.assertTrue(all(Path(log).is_file() for log in logs))
            self.assertEqual(sum(Path(log).name == "setup-clean.log" for log in logs), 2)
            self.assertEqual(sum(Path(log).name == "setup-monitor-start.log" for log in logs), 1)
            self.assertFalse(any("monitor_startup" in row["phases"] for row in rows))
            status = subprocess.run([str(binary), "monitor", "status"],
                                    cwd=root / "build-j2-retained-candidate",
                                    capture_output=True, text=True)
            self.assertNotEqual(status.returncode, 0)


if __name__ == "__main__":
    unittest.main()
