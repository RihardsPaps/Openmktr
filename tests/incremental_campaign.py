#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""GNU-free tests of the external campaign's failure detection."""
import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "incremental", Path(__file__).resolve().parents[1] / "validation/incremental.py")
incremental = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(incremental)


class ToyCampaign(incremental.Campaign):
    """An independent copy recipe with intentional faulty engine policies."""
    def build(self, tree, project, mode, label, interrupt=None):
        source = tree / "input"
        target = tree / "output"
        if (mode == "direct" or (mode == "make" and getattr(
                self, "shared_miss", False))) and target.exists():
            return  # Deliberate missed rebuild.
        if mode == "ninja" or not target.exists() or target.read_bytes() != source.read_bytes():
            target.write_bytes(source.read_bytes())  # Ninja deliberately overbuilds.


class CampaignTests(unittest.TestCase):
    def test_missed_and_unnecessary_are_distinct(self):
        self.assertEqual(incremental.classify({"a", "b"}, {"b", "c"}), {
            "missed_rebuilds": ["a"], "unnecessary_rebuilds": ["c"]})

    def test_shared_miss_is_caught_by_required_coverage(self):
        self.assertEqual(incremental.classify([], [], ["dependent"])[
            "missed_rebuilds"], ["dependent"])

    def test_shared_overbuild_is_caught_by_exact_fixture_expectations(self):
        self.assertEqual(incremental.classify(["a", "b"], ["a", "b"], ["a"])[
            "unnecessary_rebuilds"], ["b"])

    def test_differential_and_clean_oracle_detect_faults(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "input").write_text("old")
            args = argparse.Namespace(output=root / "evidence", env=[],
                                      make=sys.executable, cc=sys.executable,
                                      omktr=Path(sys.executable), jobs=2, timeout=5)
            campaign = ToyCampaign(args)
            project = dict(name="toy", source=source, artifacts=["output"])
            case = dict(name="change", expected=["output"], changes=[
                dict(path="input", action="write", text="new")])
            report = campaign.case(project, case, campaign.root / "change")
            direct = report["results"]["direct"]
            self.assertEqual(direct["missed_rebuilds"], ["output"])
            self.assertEqual(direct["stale_or_missing_outputs"], ["output"])
            self.assertTrue(direct["correctness_failure"])
            self.assertTrue(report["results"]["ninja"]["performance_defect"])
            self.assertFalse(report["results"]["make"]["correctness_failure"])
            self.assertEqual((source / "input").read_text(), "old")
            self.assertTrue((campaign.root / "change/result.json").exists())
            campaign.shared_miss = True
            case.pop("expected")
            shared = campaign.case(project, case, campaign.root / "shared-miss")
            for mode in ("make", "direct"):
                result = shared["results"][mode]
                self.assertEqual(result["missed_rebuilds"], [])
                self.assertEqual(result["stale_or_missing_outputs"], ["output"])
                self.assertTrue(result["correctness_failure"])

    def test_mutations_cannot_escape_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                incremental.contained(Path(temporary), "../outside")


if __name__ == "__main__":
    unittest.main()
