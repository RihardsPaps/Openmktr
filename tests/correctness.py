#!/usr/bin/env python3
"""Behavioral checks for build correctness across direct and Ninja modes."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


KATI = Path(os.environ.get(
    "KATI_BINARY", Path(__file__).resolve().parents[1] / "ckati"
)).resolve()


class BuildCorrectness(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kati-correctness-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def makefile(self, text):
        (self.directory / "Makefile").write_text(text, encoding="utf-8")

    def run_command(self, *args, success=True):
        result = subprocess.run(
            args, cwd=self.directory, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=15, check=False,
        )
        output = result.stdout.decode("utf-8", "replace")
        if success:
            self.assertEqual(result.returncode, 0, output)
        return result.returncode, output

    def direct(self, *args, success=True):
        return self.run_command(str(KATI), *args, success=success)

    def ninja(self):
        self.direct("--ninja", "--regen")
        return self.run_command("sh", "./ninja.sh", "-j2")

    def test_rebuilt_child_updates_parent(self):
        self.makefile("all: parent\nparent: child\n\t@cat child > parent\n"
                      "child: source\n\t@cat source > child\n")
        for name, content, stamp in (
            ("source", "new", 300), ("child", "old", 100),
            ("parent", "old", 200),
        ):
            path = self.directory / name
            path.write_text(content)
            os.utime(path, (stamp, stamp))
        self.direct("-j1")
        self.assertEqual((self.directory / "parent").read_text(), "new")

    def test_existing_phony_order_only_is_run(self):
        self.makefile("all: | prepare\n.PHONY: prepare\n"
                      "prepare:\n\t@echo ran > marker\n")
        (self.directory / "prepare").touch()
        self.direct("-j1")
        self.assertEqual((self.directory / "marker").read_text().strip(), "ran")

    def test_dry_run_keeps_intermediate(self):
        self.makefile(".INTERMEDIATE: temp\nall: final\n"
                      "final: temp\n\t@cp temp final\n"
                      "temp:\n\t@echo new > temp\n")
        (self.directory / "temp").write_text("important")
        (self.directory / "final").write_text("important")
        self.direct("-n", "all")
        self.assertEqual((self.directory / "temp").read_text(), "important")

    def test_delete_on_error_direct(self):
        self.makefile(".DELETE_ON_ERROR:\nall:\n"
                      "\t@echo partial > all\n\t@false\n")
        status, _ = self.direct("-j1", success=False)
        self.assertNotEqual(status, 0)
        self.assertFalse((self.directory / "all").exists())

    def test_failed_ninja_recipe_stays_failed(self):
        self.makefile("all:\n\t@sleep 0.05; echo partial > all; false\n")
        self.direct("--ninja")
        status, _ = self.run_command("sh", "./ninja.sh", "-j1", success=False)
        self.assertNotEqual(status, 0)

    def test_target_specific_export(self):
        self.makefile("all: export VALUE = target\nall:\n"
                      "\t@printf '%s' \"$${VALUE-unset}\" > result\n")
        self.direct("-j1")
        self.assertEqual((self.directory / "result").read_text(), "target")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "target")

    def test_oneshell_conditional_and_prefixes(self):
        self.makefile(".ONESHELL:\nall:\n\t@if true; then\n"
                      "\t  echo yes > result\n\t@fi\n")
        self.direct("-j1")
        self.assertEqual((self.directory / "result").read_text().strip(), "yes")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text().strip(), "yes")

    def test_existing_ninja_leaf_can_be_rebuilt(self):
        self.makefile("all: leaf\nleaf:\n\t@echo hi > leaf\n")
        (self.directory / "leaf").write_text("old")
        self.direct("--ninja")
        (self.directory / "leaf").unlink()
        self.run_command("sh", "./ninja.sh", "-j1")
        self.assertEqual((self.directory / "leaf").read_text().strip(), "hi")

    def test_regen_detects_missing_support_and_read_inputs(self):
        self.makefile("V := $(file <input)\nall:\n\t@echo $(V)\n")
        (self.directory / "input").write_text("hello")
        self.direct("--ninja", "--regen")
        (self.directory / "input").unlink()
        _, output = self.direct("--ninja", "--regen")
        self.assertNotIn("No need to regenerate", output)
        (self.directory / "env.sh").unlink()
        _, output = self.direct("--ninja", "--regen")
        self.assertNotIn("No need to regenerate", output)

    def test_corrupt_stamp_regenerates(self):
        self.makefile("all:\n\t@echo ok\n")
        self.direct("--ninja", "--regen")
        (self.directory / ".kati_stamp").write_bytes(
            b"\0" * 8 + b"KAT2" + b"\xff" * 4
        )
        status, output = self.direct("--ninja", "--regen", success=False)
        self.assertEqual(status, 0, output)
        self.assertNotIn("No need to regenerate", output)

    def test_missing_option_values_fail_cleanly(self):
        self.makefile("all:\n\t@true\n")
        for option in ("-j", "--remote_num_jobs", "--writable"):
            with self.subTest(option=option):
                status, output = self.direct(option, success=False)
                self.assertNotEqual(status, 0)
                self.assertGreaterEqual(status, 0, output)

    def test_parallel_missing_prerequisites_fail_cleanly(self):
        self.makefile("all: foo bar\n")
        for _ in range(10):
            status, output = self.direct("-j4", success=False)
            self.assertEqual(status, 1, output)
            self.assertIn("No rule to make target", output)

    def test_job_limit_and_nested_parallelism(self):
        self.makefile("all: group\ngroup: a b\na b:\n"
                      "\t@python3 worker.py $@\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport sys, time\n"
            "Path('start-' + sys.argv[1]).touch()\n"
            "deadline = time.monotonic() + 2\n"
            "while not (Path('start-a').exists() and Path('start-b').exists()):\n"
            "    if time.monotonic() > deadline: raise SystemExit(1)\n"
            "    time.sleep(0.01)\n"
            "Path(sys.argv[1]).touch()\n", encoding="utf-8",
        )
        self.direct("-j2")
        self.assertTrue((self.directory / "a").exists())
        self.assertTrue((self.directory / "b").exists())

    def test_one_job_never_overlaps_recipes(self):
        self.makefile("all: a b\na b:\n\t@python3 worker.py $@\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport json, sys, time\n"
            "start = time.monotonic()\n"
            "time.sleep(0.1)\n"
            "Path(sys.argv[1]).write_text(json.dumps([start, time.monotonic()]))\n",
            encoding="utf-8",
        )
        self.direct("-j1")
        first = json.loads((self.directory / "a").read_text())
        second = json.loads((self.directory / "b").read_text())
        self.assertGreaterEqual(max(first[0], second[0]),
                                min(first[1], second[1]))


if __name__ == "__main__":
    unittest.main()
