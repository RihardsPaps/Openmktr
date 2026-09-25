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

    def test_version_is_compatible_with_make_host_checks(self):
        _, output = self.direct("--version")
        self.assertRegex(output, r"\AGNU Make 4\.2\.1\nckati [^\n]+\n\Z")

        # Buildroot's check-host-make.sh extracts the first version line
        # with these sed expressions before comparing major and minor.
        version = subprocess.run(
            ["sed", "-e", r"s/^.* \([0-9\.]\)/\1/g",
             "-e", r"s/[-\n].*//g", "-e", "1q"],
            input=output, text=True, capture_output=True, check=True,
        ).stdout.strip()
        self.assertEqual(version, "4.2.1")

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

    def test_existing_output_with_order_only_prerequisite_stays_current(self):
        self.makefile(
            "all: output\n"
            "output: | prepare\n\t@printf 'rebuilt' > output\n"
            ".PHONY: prepare\n"
            "prepare:\n\t@printf 'ready' > marker\n"
        )
        (self.directory / "output").write_text("current")
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "marker").read_text(), "ready")
        self.assertEqual((self.directory / "output").read_text(), "current")
        (self.directory / "output").unlink()
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "output").read_text(), "rebuilt")

    def test_included_makefile_with_directory_prerequisite(self):
        self.makefile(
            "include build/version.mk\n"
            "all: result\n"
            "result:\n\t@printf '%s\\n' '$(version)' > $@\n"
            "build/version.mk: | build\n"
            "\t@printf 'version := ready\\n' > $@\n"
            "build:\n\t@mkdir -p $@\n"
        )
        self.direct("--ninja", "--regen")
        self.assertTrue((self.directory / "build.ninja").exists())
        self.assertEqual(
            (self.directory / "build/version.mk").read_text().strip(),
            "version := ready",
        )
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text().strip(), "ready")
        self.direct("--ninja", "--regen")

    def test_suffix_reset_keeps_first_makefile_goal_as_default(self):
        self.makefile(
            ".SUFFIXES:\n"
            "all: result\n"
            "result:\n\t@printf 'ready\\n' > $@\n"
        )
        self.direct("--ninja")
        self.assertIn("default all\n", (self.directory / "build.ninja").read_text())
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text().strip(), "ready")

    def test_ninja_directory_targets_create_outputs(self):
        self.makefile(
            "all: new/result existing/result\n"
            "new/result existing/result: %/result: | %/\n"
            "\t@printf 'ready\\n' > $@\n"
            "new/ existing/: \n\t@mkdir -p $@\n"
        )
        (self.directory / "existing").mkdir()
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        for name in ("new", "existing"):
            self.assertEqual(
                (self.directory / name / "result").read_text().strip(),
                "ready",
            )

    def test_existing_directory_alias_does_not_duplicate_ninja_output(self):
        self.makefile(
            "all: doc doc/result\n"
            "doc:\n\t@:\n"
            "doc/result: | doc/\n\t@printf 'ready\\n' > $@\n"
            "doc/: \n\t@mkdir -p $@\n"
        )
        (self.directory / "doc").mkdir()
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "doc/result").read_text().strip(), "ready")

    def test_existing_source_from_implicit_rule_is_not_cleaned(self):
        self.makefile(
            ".SUFFIXES:\n"
            "all: source.o\n"
            "%.o: %.c %.h\n\t@cp $< $@\n"
            "%.h:\n\t@:\n"
        )
        (self.directory / "source.c").write_text("source")
        (self.directory / "source.h").write_text("keep this header")
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "source.o").read_text(), "source")
        self.assertEqual(
            (self.directory / "source.h").read_text(), "keep this header"
        )
        self.run_command("sh", "./ninja.sh", "-t", "clean")
        self.assertEqual(
            (self.directory / "source.h").read_text(), "keep this header"
        )
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "source.o").read_text(), "source")

    def test_ninja_respects_current_output_before_first_build_log_entry(self):
        self.makefile(
            "all: generated\n"
            "generated: source\n\t@printf 'rebuilt' > $@\n"
        )
        (self.directory / "source").write_text("input")
        (self.directory / "generated").write_text("current")
        os.utime(self.directory / "source", (100, 100))
        os.utime(self.directory / "generated", (200, 200))
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "generated").read_text(), "current")
        os.utime(self.directory / "source", (300, 300))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "generated").read_text(), "rebuilt")

    def test_existing_file_with_rule_has_one_ninja_output_for_path_aliases(self):
        self.makefile(
            "all: result\n"
            "result: one/../source.h two/../source.h\n"
            "\t@cp source.h $@\n"
            "source.h:\n\t@:\n"
        )
        (self.directory / "one").mkdir()
        (self.directory / "two").mkdir()
        (self.directory / "source.h").write_text("keep this header")
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text(), "keep this header")
        self.assertEqual(
            (self.directory / "source.h").read_text(), "keep this header"
        )

    def test_ninja_executes_large_recipe_from_response_file(self):
        payload = "x" * 110_000
        self.makefile(
            "all: out\n"
            "out:\n\t@printf '%s' '" + payload + "' > $@\n"
        )
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "out").read_text(), payload)

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

    def test_nested_kati_in_recipe_uses_reserved_jobserver_slot(self):
        self.makefile("all:\n\t@python3 nested.py\n")
        (self.directory / "child.mk").write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" -f grandchild.mk -j1 all\n',
            encoding="utf-8",
        )
        (self.directory / "grandchild.mk").write_text(
            ".PHONY: all\nall:\n\t@printf x >> done\n",
            encoding="utf-8",
        )
        (self.directory / "nested.py").write_text(
            "import subprocess\n"
            f"for _ in range(2):\n    subprocess.run([{str(KATI)!r}, "
            "'-f', 'child.mk', '-j1', 'all'], check=True)\n",
            encoding="utf-8",
        )
        fifo = self.directory / "jobserver"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        os.write(fd, b"x")
        env = os.environ.copy()
        env["KATI_JOBSERVER_FIFO"] = str(fifo)
        env["KATI_JOBS"] = "1"
        env.pop("KATI_JOBSERVER_RESERVED", None)
        result = subprocess.run(
            [str(KATI), "-j1", "all"], cwd=self.directory, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=5, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual((self.directory / "done").read_text(), "xx")

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
