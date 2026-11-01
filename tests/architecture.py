#!/usr/bin/env python3
# Copyright 2026 Openmktr contributors
# SPDX-License-Identifier: MPL-2.0
"""Invocation, freshness, publication, and persistent-resolution contracts."""
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
import time
import unittest

BINARY = Path(os.environ.get("OMKTR_BINARY", Path(__file__).resolve().parents[1] / "omktr")).resolve()


class Architecture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="omktr-architecture-")
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def run_tool(self, *args, check=True):
        result = subprocess.run([str(BINARY), *args], cwd=self.root, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=20)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def write(self, text):
        (self.root / "Makefile").write_text(text)

    def wait_file(self, name, process):
        deadline = time.monotonic() + 10
        while not (self.root / name).exists():
            if process.poll() is not None:
                self.fail(process.communicate()[0])
            if time.monotonic() >= deadline:
                self.fail("timed out waiting for " + name)
            time.sleep(.02)

    def start(self, args):
        process = subprocess.Popen(args, cwd=self.root, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        def cleanup():
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
        self.addCleanup(cleanup)
        return process

    def current(self):
        return (self.root / "build.ninja").resolve().parent

    def test_build_rejects_nonregular_graph_without_waiting_for_a_writer(self):
        self.write("all:\n\t@touch result\n")
        (self.root / ".openmktr").mkdir()
        graph = self.root / ".openmktr/build.ninja"
        os.mkfifo(graph)
        result = subprocess.run([str(BINARY), "build", "all"], cwd=self.root,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nonregular graph", result.stderr)
        self.assertFalse((self.root / "result").exists())
        self.assertTrue(graph.is_fifo())

    def test_launcher_needs_no_fifo_shell_utilities_and_cleans_after_failure(self):
        # The launcher should require Ninja and the recipe shell, without
        # external mkfifo/rm helpers. Keep a colliding name owned by the caller.
        bindir = self.root / "bin"
        bindir.mkdir()
        (bindir / "ninja").symlink_to(shutil.which("ninja"))
        temporary = self.root / "tmp"
        temporary.mkdir()
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith(("KATI_", "MAKE", "MFLAGS"))}
        environment.update(PATH=str(bindir), TMPDIR=str(temporary))
        for status in (0, 7):
            self.write(".PHONY: all\nall:\n"
                       "\t@printf '%s' \"$$KATI_JOBSERVER_FIFO\" > fifo-path; "
                       f"exit {status}\n")
            subprocess.run([str(BINARY), "--ninja", "all"], cwd=self.root,
                           env=environment, check=True, capture_output=True)

            def occupy_name():
                (temporary / f"kati-jobserver-{os.getpid()}").write_text("owned")

            result = subprocess.run(["/bin/sh", "ninja.sh", "-j2"],
                                    cwd=self.root, env=environment,
                                    preexec_fn=occupy_name, capture_output=True,
                                    timeout=10)
            self.assertEqual(result.returncode == 0, status == 0, result.stderr)
            self.assertFalse(Path((self.root / "fifo-path").read_text()).exists())
        leftovers = list(temporary.iterdir())
        self.assertEqual(len(leftovers), 2)
        self.assertTrue(all(path.read_text() == "owned" for path in leftovers))

    def test_nanosecond_freshness_and_automatic_newer_inputs(self):
        self.write("all: output\noutput: input\n\t@printf '%s' '$?' > $@\n")
        for mode in ([], ["build"]):
            with self.subTest(mode=mode):
                (self.root / "input").write_text("source")
                (self.root / "output").write_text("old")
                base = 1_700_000_000_000_000_000
                os.utime(self.root / "output", ns=(base + 1, base + 1))
                os.utime(self.root / "input", ns=(base + 2, base + 2))
                self.run_tool(*mode, "-j4", "all")
                self.assertEqual((self.root / "output").read_text(), "input")
        # Negative wall-clock timestamps are ordinary present files.
        os.utime(self.root / "output", ns=(-1_000_000_000, -1_000_000_000))
        os.utime(self.root / "input", ns=(-999_999_999, -999_999_999))
        self.run_tool("all")
        self.assertEqual((self.root / "output").read_text(), "input")

    def test_newer_inputs_include_logical_and_phony_updates(self):
        self.write(".PHONY: force\nall: output\noutput: force input\n\t@printf '%s' '$?' > $@\n")
        (self.root / "input").write_text("source")
        for mode in ([], ["build"]):
            (self.root / "output").unlink(missing_ok=True)
            self.run_tool(*mode, "all")
            self.assertEqual((self.root / "output").read_text(), "force input")
            self.run_tool(*mode, "all")
            self.assertEqual((self.root / "output").read_text(), "force")
        self.write("all: output\noutput: empty\n\t@printf '%s' '$?' > $@\nempty:\n")
        for mode in ([], ["build"]):
            (self.root / "output").write_text("existing")
            self.run_tool(*mode, "all")
            self.assertEqual((self.root / "output").read_text(), "empty")

    def test_low_resolution_freshness_and_newer_inputs(self):
        # Expected results checked against GNU make separately; the portable
        # gate requires no GNU tools. Only the output comparison is rounded;
        # prerequisites retain their timestamps. Test both orders and saved graphs.
        cases = (
            ("low", "high low", 600_000_000, 900_000_000, 400_000_000, "high low"),
            ("low", "low high", 600_000_000, 900_000_000, 400_000_000, "low high"),
            ("low", "low", 0, 0, 500_000_000, "old"),
            ("output", "high", 600_000_000, 0, 0, "old"),
            ("output", "high", 1_000_000_000, 0, 0, "high"),
            ("output low", "high low", 600_000_000, 0, 0, "old"),
            ("output", "force high", 600_000_000, 0, 0, "force"),
            ("low", "force low", 0, 0, 500_000_000, "force"),
        )
        root = self.root
        for index, (listed, inputs, high, low, output, expected) in enumerate(cases):
            for colon in (":", "::"):
                for mode in ("direct", "build", "legacy"):
                    for jobs in (1, 4):
                        with self.subTest(case=index, colon=colon, mode=mode, jobs=jobs):
                            self.root = root / f"{index}-{colon.count(':')}-{mode}-{jobs}"
                            self.root.mkdir()
                            self.write(f".LOW_RESOLUTION_TIME: {listed}\n"
                                       ".PHONY: force\nforce:\nall: output\n"
                                       f"output{colon} {inputs}\n\t@printf '%s' '$?' > $@\n")
                            for name in ("high", "low", "output", "force"):
                                (self.root / name).write_text("old")
                            if mode == "legacy":
                                self.run_tool("--ninja", "all")
                            for _ in range(2):
                                (self.root / "output").write_text("old")
                                base = 1_700_000_000_000_000_000
                                for name, offset in (("high", high), ("low", low), ("output", output)):
                                    os.utime(self.root / name, ns=(base + offset, base + offset))
                                args = ["-j" + str(jobs), "all"]
                                if mode == "legacy":
                                    subprocess.run(["sh", "ninja.sh", *args], cwd=self.root,
                                                   check=True, capture_output=True, timeout=20)
                                else:
                                    self.run_tool(*(["build"] if mode == "build" else []), *args)
                                self.assertEqual((self.root / "output").read_text(), expected)
        self.root = root

    def test_low_resolution_double_colon_groups_keep_independent_freshness(self):
        self.write(".LOW_RESOLUTION_TIME: output\nall: output\n"
                   "output:: same\n\t@$(error stale group expanded)\n"
                   "output:: newer | order\n\t@printf 'newer:%s\\n' '$?|$<|$^|$+|$|' >> trace\n"
                   ".PHONY: order\norder:\n")
        base = 1_700_000_000_000_000_000
        for name, offset in (("output", 0), ("same", 500_000_000), ("newer", 1_000_000_000)):
            (self.root / name).write_text("old")
            os.utime(self.root / name, ns=(base + offset, base + offset))
        self.run_tool("--ninja", "all")
        for mode in ("direct", "build", "legacy"):
            for jobs in (1, 4):
                with self.subTest(mode=mode, jobs=jobs):
                    (self.root / "trace").unlink(missing_ok=True)
                    if mode == "legacy":
                        subprocess.run(["sh", "ninja.sh", f"-j{jobs}", "all"], cwd=self.root,
                                       check=True, capture_output=True, timeout=20)
                    else:
                        self.run_tool(*(["build"] if mode == "build" else []), f"-j{jobs}", "all")
                    self.assertEqual((self.root / "trace").read_text(), "newer:newer|newer|newer|newer|order\n")

    def test_unconditional_double_colon_runs_beside_current_and_order_only_groups(self):
        self.write("all: output\noutput:: older\n\t@$(error current group expanded)\n"
                   "output:: | order\n\t@$(error order-only group expanded)\n"
                   "output::\n\t@printf 'run:%s\\n' '$?|$<|$^|$+|$|' >> trace\n"
                   ".PHONY: order\norder:\n")
        for name, timestamp in (("older", 100), ("output", 200)):
            (self.root / name).write_text("old")
            os.utime(self.root / name, (timestamp, timestamp))
        self.run_tool("--ninja", "all")
        for mode in ("direct", "build", "legacy"):
            for repeat in range(2):
                with self.subTest(mode=mode, repeat=repeat):
                    (self.root / "trace").unlink(missing_ok=True)
                    if mode == "legacy":
                        subprocess.run(["sh", "ninja.sh", "-j4", "all"], cwd=self.root,
                                       check=True, capture_output=True, timeout=20)
                    else:
                        self.run_tool(*(["build"] if mode == "build" else []), "-j4", "all")
                    self.assertEqual((self.root / "trace").read_text(), "run:||||\n")

    def test_double_colon_automatic_inputs_keep_vpath_providers(self):
        (self.root / "sources").mkdir()
        for name in ("first", "second", "order"):
            (self.root / "sources" / name).write_text(name)
        self.write("VPATH := sources\nall: output\n"
                   "output:: first first | order\n\t@printf '%s\\n' '$?|$<|$^|$+|$|' >> trace\n"
                   "output:: second\n\t@printf '%s\\n' '$?|$<|$^|$+|$|' >> trace\n")
        self.run_tool("--ninja", "all")
        expected = ("sources/first|sources/first|sources/first|sources/first sources/first|sources/order\n"
                    "sources/second|sources/second|sources/second|sources/second|\n")
        for mode in ("direct", "build", "legacy"):
            with self.subTest(mode=mode):
                (self.root / "trace").unlink(missing_ok=True)
                if mode == "legacy":
                    subprocess.run(["sh", "ninja.sh", "-j4", "all"], cwd=self.root,
                                   check=True, capture_output=True, timeout=20)
                else:
                    self.run_tool(*(["build"] if mode == "build" else []), "-j4", "all")
                self.assertEqual((self.root / "trace").read_text(), expected)

    def test_runtime_groups_keep_recipe_less_vpath_aliases(self):
        (self.root / "sources").mkdir()
        (self.root / "sources/input").write_text("provider")
        (self.root / "sources/order").write_text("order")
        self.write("VPATH := sources\nall: output\ninput:\norder:\n"
                   "output:: input | order\n\t@cat $< > $@; printf '%s' '$?|$|' > trace\n")
        self.run_tool("--ninja", "all")
        for mode in ("direct", "build", "legacy"):
            with self.subTest(mode=mode):
                (self.root / "output").unlink(missing_ok=True)
                if mode == "legacy":
                    subprocess.run(["sh", "ninja.sh", "-j4", "all"], cwd=self.root,
                                   check=True, capture_output=True, timeout=20)
                else:
                    self.run_tool(*(["build"] if mode == "build" else []), "-j4", "all")
                self.assertEqual((self.root / "output").read_text(), "provider")
                self.assertEqual((self.root / "trace").read_text(), "sources/input|sources/order")
                self.assertFalse((self.root / "input").exists())

    def test_runtime_groups_keep_lexical_phony_and_logical_metadata(self):
        root = self.root
        for kind in ("phony", "phony-file", "logical"):
            for mode in ("direct", "build", "legacy"):
                with self.subTest(kind=kind, mode=mode):
                    self.root = root / (kind + "-" + mode)
                    self.root.mkdir()
                    (self.root / "dir").mkdir()
                    self.write((".PHONY: dir/../force\n" if kind.startswith("phony") else "") +
                               "all: output\ndir/../force:; @:\n"
                               "output:: dir/../force\n\t@printf '%s' '$?' > $@\n")
                    if kind == "phony-file":
                        (self.root / "force").write_text("existing")
                    (self.root / "output").write_text("old")
                    if mode == "legacy":
                        self.run_tool("--ninja", "all")
                        subprocess.run(["sh", "ninja.sh", "-j4", "all"], cwd=self.root,
                                       check=True, capture_output=True, timeout=20)
                    else:
                        self.run_tool(*(["build"] if mode == "build" else []), "-j4", "all")
                    self.assertEqual((self.root / "output").read_text(), "dir/../force")
        self.root = root

    def test_expansion_failure_cancels_and_reaps_active_siblings(self):
        self.write(".PHONY: all fail ready\nall: output fail\noutput:\n"
                   "\t@echo $$$$ > child; printf partial > output; touch started; sleep 30\n"
                   "ready:\n\t@while test ! -f started; do sleep .01; done\n"
                   "fail: ready\n\t@$(error expansion failed)\n")
        started = time.monotonic()
        result = self.run_tool("-j2", "all", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expansion failed", result.stdout)
        self.assertLess(time.monotonic() - started, 5)
        self.assertFalse((self.root / "output").exists())
        pid = int((self.root / "child").read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_abandoned_generation_does_not_replace_complete_bundle(self):
        self.write("export WORD = old\nall:\n\t@printf '%s' \"$$WORD\" > result\n")
        self.run_tool("--ninja", "all")
        old = self.current()
        graph = (old / "build.ninja").read_bytes()
        self.write("export WORD = new\nall: .KATI_TAGS = $(shell touch staging; while test ! -f release; do sleep .02; done)\n"
                   "all:\n\t@printf '%s' \"$$WORD\" > result\n")
        process = self.start([str(BINARY), "--ninja", "all"])
        self.wait_file("staging", process)
        self.assertEqual(self.current(), old)
        self.assertEqual((self.root / "build.ninja").read_bytes(), graph)
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate(timeout=5)
        subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True, capture_output=True, timeout=10)
        self.assertEqual((self.root / "result").read_text(), "old")
        (self.root / "release").touch()
        self.run_tool("--ninja", "all")
        subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True, capture_output=True, timeout=10)
        self.assertEqual((self.root / "result").read_text(), "new")
        self.assertNotEqual(self.current(), old)

    def test_reader_pins_bundle_across_multiple_publications(self):
        for command in (["sh", "ninja.sh"],
                        [str(BINARY), "build", "--ninja_dir=.", "all"]):
            with self.subTest(command=command):
                for marker in ("running", "release"):
                    (self.root / marker).unlink(missing_ok=True)
                self.write("export WORD = old\n.PHONY: all\nall:\n"
                           "\t@touch running; while test ! -f release; do sleep .02; done; printf '%s' \"$$WORD\" > result\n")
                self.run_tool("--ninja", "all")
                reader = self.start(command)
                self.wait_file("running", reader)
                old = self.current()
                for value in ("new", "newest", "latest"):
                    self.write("export WORD = " + value + "\n.PHONY: all\nall:\n\t@printf '%s' \"$$WORD\" > result\n")
                    self.run_tool("--ninja", "all")
                self.assertTrue(old.exists())
                (self.root / "release").touch()
                output = reader.communicate(timeout=10)[0]
                self.assertEqual(reader.returncode, 0, output)
                self.assertEqual((self.root / "result").read_text(), "old")
                self.run_tool("--ninja", "all")
                self.assertFalse(old.exists())

    def test_native_launch_plan_damage_fails_closed_and_build_repairs_it(self):
        self.write("all: output\noutput:\n\t@printf stable > $@\n")
        public = self.root / "ninja.sh.launch"
        public.write_text("caller-owned")
        for damage in (None, b"partial record"):
            self.run_tool("--ninja", "all")
            self.assertEqual(public.read_text(), "caller-owned")
            plan = self.current() / "ninja.sh.launch"
            if damage is None:
                plan.unlink()
            else:
                plan.write_bytes(damage)
            result = subprocess.run(["sh", "ninja.sh"], cwd=self.root,
                                    capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "output").exists())
            self.run_tool("build", "--ninja_dir=.", "all")
            self.assertEqual((self.root / "output").read_text(), "stable")
            (self.root / "output").unlink()

    def test_unknown_and_corrupt_generation_manifests_regenerate(self):
        self.write("all: output\noutput:\n\t@printf stable > output\n")
        self.run_tool("build", "all")
        graph = self.root / ".openmktr/build.ninja"
        for damage in (b"unknown version", b"", b"partial\x00record"):
            before = graph.resolve().parent
            (before / "manifest").write_bytes(damage)
            self.run_tool("build", "all")
            self.assertNotEqual(graph.resolve().parent, before)
            self.assertEqual((self.root / "output").read_text(), "stable")

    def test_packed_recipe_change_preserves_neighbor_commands_and_saved_graph(self):
        self.write("all: first second\nfirst:\n\t@printf old > $@\n"
                   "second:\n\t@printf unchanged > $@\n")
        self.run_tool("--ninja", "all")
        saved = self.root / "saved.ninja"
        saved.write_bytes((self.root / "build.ninja").read_bytes())
        subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True, capture_output=True)
        before = (self.root / "second").stat().st_mtime_ns
        self.assertTrue(list((self.root / ".kati_recipes").glob("*.packed-*")))
        self.write("all: first second\nfirst:\n\t@printf changed > $@\n"
                   "second:\n\t@printf unchanged > $@\n")
        self.run_tool("--ninja", "all")
        subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True, capture_output=True)
        self.assertEqual((self.root / "first").read_text(), "changed")
        self.assertEqual((self.root / "second").stat().st_mtime_ns, before)
        for _ in range(3):
            self.run_tool("--ninja", "all")
        (self.root / "first").unlink()
        subprocess.run(["ninja", "-f", "saved.ninja", "first"], cwd=self.root,
                       check=True, capture_output=True)
        self.assertEqual((self.root / "first").read_text(), "old")

    def test_missing_or_corrupt_recipe_pack_fails_before_execution(self):
        self.write("all: output\noutput:\n\t@printf stable > $@\n")
        for corrupt in (False, True):
            self.run_tool("--ninja", "all")
            packs = list((self.root / ".kati_recipes").glob(".pack-*"))
            self.assertTrue(packs)
            for pack in packs:
                if corrupt:
                    pack.chmod(0o600)
                    pack.write_bytes(b"corrupted")
                else:
                    pack.unlink()
            result = subprocess.run(["sh", "ninja.sh"], cwd=self.root,
                                    capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "output").exists())
            self.run_tool("--ninja", "all")
            subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True, capture_output=True)
            self.assertEqual((self.root / "output").read_text(), "stable")
            (self.root / "output").unlink()

    def test_raw_ninja_keeps_immutable_recipe_objects_after_collection(self):
        (self.root / "source").write_text("old")
        self.write(".INTERMEDIATE: middle\nall: output\n"
                   "middle: source\n\t@touch running; while test ! -f release; do sleep .02; done; cp source $@\n"
                   "output: middle\n\t@cp middle $@\n")
        self.run_tool("--ninja", "all")
        old = self.current()
        reader = self.start(["ninja", "-f", "build.ninja", "-j2"])
        self.wait_file("running", reader)
        for _ in range(3):
            self.run_tool("--ninja", "all")
        self.assertFalse(old.exists())
        (self.root / "release").touch()
        output = reader.communicate(timeout=10)[0]
        self.assertEqual(reader.returncode, 0, output)
        self.assertEqual((self.root / "output").read_text(), "old")

    def test_legacy_files_migrate_without_manual_cache_removal(self):
        self.write("all: output\noutput:\n\t@printf stable > output\n")
        self.run_tool("--ninja", "all")
        generation_root = self.current().parent
        previous_graph = (self.root / "build.ninja").read_bytes()
        for name in ("build.ninja", "env.sh", "ninja.sh", ".kati_stamp"):
            path = self.root / name
            content = path.read_bytes()
            path.unlink()
            path.write_bytes(content)
        # Exercise first migration from standalone files, not merely repair
        # aliases while an existing generation remains published.
        (generation_root / "current").unlink()
        self.run_tool("--ninja", "--regen", "all")
        self.assertEqual((generation_root / "legacy/build.ninja").read_bytes(),
                         previous_graph)
        self.assertTrue((self.root / "build.ninja").is_symlink())
        subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True, capture_output=True, timeout=10)
        self.assertEqual((self.root / "output").read_text(), "stable")

    def test_missing_environment_object_regenerates_or_fails_before_recipe(self):
        self.write("export WORD = retained\n.PHONY: all\nall:\n\t@printf '%s' \"$$WORD\" > result\n")
        self.run_tool("build", "all")
        for path in (self.root / ".openmktr/.omktr-objects").glob("env-*"):
            path.unlink()
        self.run_tool("build", "all")
        self.assertEqual((self.root / "result").read_text(), "retained")
        self.run_tool("--ninja", "all")
        for path in (self.root / ".omktr-objects").glob("env-*"):
            path.unlink()
        result = subprocess.run(["ninja", "-f", "build.ninja"], cwd=self.root,
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / "result").read_text(), "retained")

    def test_implicit_resolution_revalidates_positive_and_negative_witnesses(self):
        self.write(".INTERMEDIATE: result.generated\nall: result.o\n"
                   "%.o: %.generated\n\t@cp $< $@\n"
                   "%.generated: %.source\n\t@cp $< $@\n")
        (self.root / "result.source").write_text("one")
        trace = self.root / "stats.jsonl"
        self.run_tool("--stats_json", str(trace), "all")
        self.run_tool("--stats_json", str(trace), "all")
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        self.assertGreater(records[-1]["counters"].get("resolution_hits", 0), 0)
        (self.root / "result.source").write_text("two")
        self.run_tool("--stats_json", str(trace), "all")
        self.assertEqual((self.root / "result.o").read_text(), "two")
        (self.root / "result.o").unlink()
        (self.root / "result.source").unlink()
        self.assertNotEqual(self.run_tool("all", check=False).returncode, 0)
        (self.root / "result.source").write_text("three")
        self.run_tool("all")
        self.assertEqual((self.root / "result.o").read_text(), "three")
        (self.root / ".kati_resolution").write_bytes(b"obsolete cache")
        self.run_tool("all")
        self.assertEqual((self.root / "result.o").read_text(), "three")


if __name__ == "__main__":
    unittest.main()
