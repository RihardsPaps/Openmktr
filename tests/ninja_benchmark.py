#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Portable checks for paired Ninja performance evidence accounting."""

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "benchmark_ninja", Path(__file__).resolve().parents[1] / "validation/benchmark_ninja.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class Evidence(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
