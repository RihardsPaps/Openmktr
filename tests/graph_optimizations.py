#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Behavioral coverage for graph reuse, imports, monitors and output publication."""
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import struct
import re
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(os.environ.get("OMKTR_BINARY", Path(__file__).resolve().parents[1] / "omktr")).resolve()


class GraphOptimizations(unittest.TestCase):
    def test_dependency_fragments_recheck_directory_aliases_after_effects(self):
        (self.root / "dir").mkdir()
        self.makefile(
            ".PHONY: all dir\nall: a b dir\na b: dir/\n"
            "a: .KATI_TAGS = $(if $(shell rmdir dir),removed,removed)\n"
            "a:\n\t@printf a > $@\nb:\n\t@test -d dir && printf b > $@\n"
            "dir:\n\t@mkdir -p dir\ndir/:\n\t@mkdir -p dir\n")
        # Visit the existing directory alias before the tag removes it.
        self.run_tool("--ninja", "-j32", "--graph_optimizations=aggressive",
                      "dir/", "all")
        graph = (self.root / "build.ninja").read_text()
        self.assertRegex(graph, r"(?m)^build a: \S+$")
        self.assertRegex(graph, r"(?m)^build b: \S+ dir/$")
        subprocess.run(["ninja", "-j32"], cwd=self.root, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=30)
        self.assertEqual((self.root / "a").read_text(), "a")
        self.assertEqual((self.root / "b").read_text(), "b")

    def test_shared_prerequisite_fragments_preserve_duplicates_and_target_scopes(self):
        (self.root / "first").write_text("ready")
        (self.root / "second").write_text("unused")
        self.makefile(
            "all: a b\na: WORD = alpha\nb: WORD = beta\n"
            "a b: first second first\n"
            "\t@printf '%s' '$^|$+|$<|$(WORD)|$(shell cat first)' > $@\n")
        for mode in ("direct", "build"):
            with self.subTest(mode=mode):
                for name in ("a", "b"):
                    (self.root / name).unlink(missing_ok=True)
                args = ["-j32", "--graph_optimizations=aggressive", "all"]
                if mode == "build":
                    args.insert(0, "build")
                self.run_tool(*args)
                for name, word in (("a", "alpha"), ("b", "beta")):
                    self.assertEqual((self.root / name).read_text(),
                                     f"first second|first second first|first|{word}|ready")
        (self.root / "first").write_text("changed")
        self.run_tool("build", "-j32", "--graph_optimizations=aggressive", "all")
        for name, word in (("a", "alpha"), ("b", "beta")):
            self.assertEqual((self.root / name).read_text(),
                             f"first second|first second first|first|{word}|changed")

    def test_single_expression_admission_is_bounded_and_keeps_nested_effects(self):
        words = " ".join(str(i) for i in range(70000))
        self.makefile(
            "VALUES := $(foreach item," + words + ","
            "$(if $(filter 69999,$(item)),$(info EXPANSION-END))"
            "$(subst x,y,x$(item)))\n"
            "$(eval LAST := $(lastword $(VALUES)))\n"
            "all:\n\t@printf '%s' '$(words $(VALUES)):$(firstword $(VALUES)):$(LAST)' > result\n")
        output = self.run_tool("build", "-j32", "--graph_optimizations=aggressive",
                               "--kati_stats", "all").stdout
        self.assertLess(output.index("evaluation recording disabled"),
                        output.index("EXPANSION-END"))
        match = re.search(r"recording disabled after (\d+) hits and (\d+) misses", output)
        self.assertIsNotNone(match, output)
        self.assertLessEqual(int(match[1]) + int(match[2]), 65536)
        self.assertEqual((self.root / "result").read_text(), "70000:y0:y69999")
        self.run_tool("-j32", "all")
        self.assertEqual((self.root / "result").read_text(), "70000:y0:y69999")

    def test_unproductive_recording_falls_back_without_losing_effects(self):
        (self.root / "bulk.mk").write_text(
            "".join(f"V{i} := $(subst X,Y,X{i})\n" for i in range(70000)) +
            "$(info ADMISSION-NESTED-END)\n")
        self.makefile("$(eval include bulk.mk)\n"
                      "$(eval EFFECT := $(V69999))\n"
                      "all:\n\t@printf '%s' '$(V0):$(EFFECT)' > result\n")
        output = self.run_tool("build", "-j32", "--graph_optimizations=aggressive",
                               "--kati_stats", "all").stdout
        self.assertIn("evaluation recording disabled", output)
        self.assertLess(output.index("evaluation recording disabled"),
                        output.index("ADMISSION-NESTED-END"))
        self.assertEqual((self.root / "result").read_text(), "Y0:Y69999")
        output = self.run_tool("-j32", "--graph_optimizations=aggressive", "all").stdout
        self.assertEqual((self.root / "result").read_text(), "Y0:Y69999")

    def test_compact_absolute_paths_preserve_outputs(self):
        directory = self.root / ("long-directory-" * 4) / "build"
        directory.mkdir(parents=True)
        target = directory / "output"
        (directory / "Makefile").write_text(
            f"all: {target}\n{target}:\n\t@printf done > $@\n")
        for profile in ("off", "aggressive"):
            if target.exists():
                target.unlink()
            subprocess.run([str(BINARY), "build", "-j32",
                            "--graph_optimizations=" + profile], cwd=directory,
                           capture_output=True, check=True, timeout=30)
            self.assertEqual(target.read_text(), "done")
            graph = (directory / ".openmktr/build.ninja").read_text()
            self.assertEqual("omktr_path_" in graph, profile == "aggressive")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="omktr-graph-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_tool(self, *args, check=True, **kwargs):
        return subprocess.run([str(BINARY), *args], cwd=self.root, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              check=check, timeout=30, **kwargs)

    def makefile(self, source):
        (self.root / "Makefile").write_text(source)

    def test_shared_dependency_waiters_keep_the_recipe_budget_busy(self):
        worker = '''import fcntl,json,sys,time
from pathlib import Path
expected=int(sys.argv[2])
def update(change):
 with open('counter','a+') as stream:
  fcntl.flock(stream,fcntl.LOCK_EX);stream.seek(0);data=stream.read()
  value=json.loads(data) if data else dict(active=0,peak=0,started=0,release=False,deadline=time.monotonic()+2)
  result=change(value);stream.seek(0);stream.truncate();json.dump(value,stream)
  return result
def enter(value):
 value['active']+=1;value['started']+=1;value['peak']=max(value['peak'],value['active'])
 value['release']=value['release'] or value['peak']>=expected
update(enter)
def ready(value):
 value['release']=value['release'] or time.monotonic()>=value['deadline']
 return value['release']
while not update(ready):time.sleep(.01)
time.sleep(.02)
update(lambda value:value.update(active=value['active']-1))
Path(sys.argv[1]).write_text('built')
'''
        for mode, jobs, serialized in (("direct", 1, False), ("direct", 8, False),
                                       ("direct", 16, False), ("build", 8, False),
                                       ("build", 16, False), ("recursive", 8, False),
                                       ("recursive", 16, False), ("direct", 8, True),
                                       ("direct-cycle", 8, False),
                                       ("recursive-cycle", 8, False)):
            with self.subTest(mode=mode, jobs=jobs, serialized=serialized):
                build = self.root / f"{mode}-{jobs}-{serialized}"
                build.mkdir()
                outputs = build
                execution_mode = mode.removesuffix("-cycle")
                if execution_mode == "recursive":
                    outputs = build / "child"
                    outputs.mkdir()
                    (build / "Makefile").write_text(
                        "all:\n\t@for directory in child; do $(MAKE) -C $$directory all || exit $$?; done\n")
                (outputs / "worker.py").write_text(worker)
                expected = 1 if serialized else jobs
                aliases = [f"alias{i}" for i in range(32)]
                leaves = [f"leaf{i}" for i in range(16)]
                source = ".NOTPARALLEL:\n" if serialized else ""
                source += "all: " + " ".join(aliases) + " consumer\n"
                source += "consumer: alias0\n\t@printf done > $@\n"
                for i, alias in enumerate(aliases):
                    source += f"{alias}: shared extra{i}\n\t@printf done > $@\n"
                    source += f"extra{i}:\n\t@:\n"
                source += "shared: " + " ".join(leaves) + "\n\t@printf done > $@\n"
                if mode.endswith("-cycle"):
                    source += "shared: cycle-a\ncycle-a: cycle-b\ncycle-b: cycle-a\n"
                source += " ".join(leaves) + f":\n\t@python worker.py $@ {expected}\n"
                (outputs / "Makefile").write_text(source)
                command = [str(BINARY), *(["build"] if execution_mode != "direct" else []),
                           f"-j{jobs}", "all", "--kati_stats"]
                if execution_mode == "recursive":
                    command.append("--graph_optimizations=aggressive")
                result = subprocess.run(command, cwd=build, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=30, check=True)
                if not mode.endswith("-cycle"):
                    self.assertNotIn("Circular", result.stdout)
                counter = json.loads((outputs / "counter").read_text())
                self.assertEqual(counter["started"], 16)
                self.assertEqual(counter["active"], 0)
                self.assertEqual(counter["peak"], expected)
                for leaf in leaves:
                    self.assertEqual((outputs / leaf).read_text(), "built")
                self.assertEqual((outputs / "consumer").read_text(), "done")

    def test_write_if_changed_content_mode_and_failures(self):
        self.run_tool("write-if-changed", "output", "--", "printf", "one")
        output = self.root / "output"
        output.chmod(0o640)
        before = output.stat()
        self.run_tool("write-if-changed", "output", "--", "printf", "one")
        self.assertEqual(before.st_mtime_ns, output.stat().st_mtime_ns)
        self.assertEqual(output.stat().st_mode & 0o777, 0o640)
        failed = self.run_tool("write-if-changed", "output", "--", "sh", "-c",
                               "printf broken; printf error >&2; exit 7", check=False)
        self.assertEqual(failed.returncode, 7)
        self.assertIn("error", failed.stdout)
        self.assertEqual(output.read_text(), "one")
        self.run_tool("write-if-changed", "output", "--", "printf", "two")
        self.assertEqual(output.read_text(), "two")
        self.assertEqual(output.stat().st_mode & 0o777, 0o640)
        self.assertEqual(list(self.root.glob("*.tmp.*")), [])
        output.chmod(0o4640)
        self.run_tool("write-if-changed", "output", "--", "printf", "special mode")
        self.assertEqual(output.stat().st_mode & 0o7777, 0o4640)

    def test_write_if_changed_shares_the_recursive_recipe_budget(self):
        worker = '''import fcntl,json,sys,time
from pathlib import Path
path=Path('../counter')
def update(change):
 with path.open('a+') as stream:
  fcntl.flock(stream,fcntl.LOCK_EX);stream.seek(0);data=stream.read()
  value=json.loads(data) if data else dict(active=0,peak=0,started=0,release=False,deadline=time.monotonic()+2)
  result=change(value);stream.seek(0);stream.truncate();json.dump(value,stream)
  return result
def enter(value):
 value['active']+=1;value['started']+=1;value['peak']=max(value['peak'],value['active'])
 value['release']=value['release'] or value['peak']>int(sys.argv[2])
update(enter)
if sys.argv[1]=='helper':Path('../helper-ready').touch()
def ready(value):
 value['release']=value['release'] or time.monotonic()>=value['deadline']
 return value['release']
while not update(ready):time.sleep(.01)
time.sleep(.03)
update(lambda value:value.update(active=value['active']-1))
print(sys.argv[1])
'''
        for jobs in (1, 4, 8, 16):
            with self.subTest(jobs=jobs):
                build = self.root / str(jobs)
                build.mkdir()
                (build / "mid").mkdir()
                (build / "leaf").mkdir()
                (build / "worker.py").write_text(worker)
                (build / "Makefile").write_text(
                    "all:\n\t@for directory in mid; do $(MAKE) -C $$directory all; done\n")
                (build / "mid/Makefile").write_text(
                    "all: helper child\nhelper:\n"
                    f"\t@$(MAKE) write-if-changed output -- python ../worker.py helper {jobs}\n"
                    "child:\n\t@while [ ! -f ../helper-ready ]; do sleep .01; done; "
                    "$(MAKE) -C ../leaf all\n")
                leaves = " ".join(f"leaf{i}" for i in range(16))
                (build / "leaf/Makefile").write_text(
                    f"all: {leaves}\n{leaves}:\n\t@python ../worker.py $@ {jobs}\n")
                subprocess.run([str(BINARY), "build", f"-j{jobs}", "all"], cwd=build,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=30, check=True)
                counter = json.loads((build / "counter").read_text())
                self.assertEqual(counter["started"], 17)
                self.assertEqual(counter["active"], 0)
                self.assertLessEqual(counter["peak"], jobs)
                self.assertEqual((build / "mid/output").read_text(), "helper\n")

    @unittest.skipUnless(sys.platform.startswith("linux"), "uses Linux process descriptors")
    def test_write_if_changed_interrupts_while_waiting_for_a_job_slot(self):
        output = self.root / "output"
        output.write_text("previous")
        fifo = self.root / "budget"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        env = dict(os.environ, KATI_JOBSERVER_FIFO=str(fifo), KATI_JOBSERVER_RESERVED="0")
        process = subprocess.Popen([str(BINARY), "write-if-changed", "output", "--", "sh", "-c",
                                    "touch producer-started; printf changed"], cwd=self.root,
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        opened = False
        for _ in range(500):
            if process.poll() is not None:
                break
            for descriptor in Path(f"/proc/{process.pid}/fd").glob("*"):
                try:
                    opened |= os.readlink(descriptor) == str(fifo)
                except FileNotFoundError:
                    pass
            if opened:
                break
            time.sleep(.01)
        self.assertTrue(opened, "helper did not enter jobserver admission")
        process.send_signal(signal.SIGINT)
        process.communicate(timeout=5)
        self.assertEqual(process.returncode, 130)
        self.assertEqual(output.read_text(), "previous")
        self.assertFalse((self.root / "producer-started").exists())
        self.assertEqual(list(self.root.glob("output.tmp.*")), [])

    def test_write_if_changed_returns_failed_and_inherited_job_slots(self):
        output = self.root / "output"
        output.write_text("previous")
        before = output.stat().st_mtime_ns
        fifo = self.root / "budget"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        os.write(fd, b"x")
        env = dict(os.environ, KATI_JOBSERVER_FIFO=str(fifo), KATI_JOBSERVER_RESERVED="0")
        failed = self.run_tool("write-if-changed", "output", "--", "sh", "-c",
                               "printf broken; exit 7", env=env, check=False)
        self.assertEqual(failed.returncode, 7)
        self.assertEqual(output.read_text(), "previous")
        self.assertEqual(output.stat().st_mtime_ns, before)
        self.assertEqual(os.read(fd, 1), b"x")
        env["KATI_JOBSERVER_RESERVED"] = "1"
        self.run_tool("write-if-changed", "output", "--", "printf", "stable", env=env)
        self.assertEqual(output.read_text(), "stable")
        with self.assertRaises(BlockingIOError):
            os.read(fd, 1)

    def test_write_if_changed_rejects_nonregular_outputs(self):
        (self.root / "target").write_text("untouched")
        (self.root / "link").symlink_to("target")
        (self.root / "directory").mkdir()
        for name in ("link", "directory"):
            self.assertNotEqual(self.run_tool("write-if-changed", name, "--", "printf",
                                              "changed", check=False).returncode, 0)
        self.assertEqual((self.root / "target").read_text(), "untouched")

    def test_write_if_changed_comparison_failure_preserves_previous_output(self):
        output = self.root / "output"
        output.write_text("previous")
        output.chmod(0)
        before = output.stat().st_mtime_ns
        binary = self.root / "helper"
        shutil.copyfile(BINARY, binary)
        binary.chmod(0o755)
        self.root.chmod(0o777)
        kwargs = {"preexec_fn": lambda: os.setuid(65534)} if os.geteuid() == 0 else {}
        result = subprocess.run([str(binary), "write-if-changed", "output", "--", "printf", "new"],
                                cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=10, **kwargs)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"cannot compare", result.stdout)
        output.chmod(0o600)
        self.assertEqual(output.read_text(), "previous")
        self.assertEqual(output.stat().st_mtime_ns, before)
        self.assertEqual(list(self.root.glob("output.tmp.*")), [])

    def test_write_if_changed_restat_prunes_only_unchanged_downstream_work(self):
        (self.root / "input").write_text("one")
        self.makefile(".PHONY: all force\nall: output\n.KATI_RESTAT: generated\n"
                      "generated: force input\n"
                      "\t@$(MAKE) write-if-changed generated -- cat input\n"
                      "output: generated\n\t@cp generated output\n")
        self.run_tool("build")
        before = {name: (self.root / name).stat().st_mtime_ns for name in ("generated", "output")}
        self.run_tool("build")
        self.assertEqual(before, {name: (self.root / name).stat().st_mtime_ns for name in before})
        (self.root / "input").write_text("two")
        self.run_tool("build")
        for name in before:
            self.assertEqual((self.root / name).read_text(), "two")
            self.assertNotEqual((self.root / name).stat().st_mtime_ns, before[name])

    def test_compact_rules_preserve_expanded_commands(self):
        self.makefile(".PHONY: all\nall: a b\na:\n\t@printf 'a $$HOME' > a\n"
                      "b:\n\t@printf 'b $$HOME' > b\n")
        self.run_tool("--ninja", "--graph_optimizations=off")
        plain = subprocess.check_output(["ninja", "-t", "commands"], cwd=self.root)
        self.run_tool("--ninja", "--graph_optimizations=safe")
        compact = subprocess.check_output(["ninja", "-t", "commands"], cwd=self.root)
        # Profiles are carried by immutable environment snapshots. The
        # serialization comparison excludes only that content-selected path.
        normalize = lambda value: re.sub(rb"/env-[0-9]+(?:\.[0-9]+)?", b"/env-SNAPSHOT", value)
        self.assertEqual(normalize(plain), normalize(compact))
        graph = (self.root / "build.ninja").read_text()
        self.assertEqual(graph.count("\nrule "), 1)
        self.assertEqual(graph.count(" --internal-run-recipe "), 1)
        self.assertNotIn("--internal-run-recipe", "\n".join(
            line for line in graph.splitlines() if line.startswith(" omktr_command =")))
        subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual((self.root / "a").read_text(), "a $HOME")

    def test_write_if_changed_interruption_preserves_existing_output(self):
        (self.root / "output").write_text("original")
        command = [str(BINARY), "write-if-changed", "output", "--", "sh", "-c",
                   "printf partial; : > started; sleep 30"]
        process = subprocess.Popen(command, cwd=self.root, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / "started").exists():
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 128 + signal.SIGTERM)
            self.assertEqual((self.root / "output").read_text(), "original")
            self.assertEqual(list(self.root.glob("*.tmp.*")), [])
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_monitor_overflow_requires_ordinary_validation(self):
        limit = Path("/proc/sys/fs/inotify/max_queued_events")
        if not limit.exists() or int(limit.read_text()) > 65536:
            self.skipTest("bounded Linux inotify overflow injection unavailable")
        self.addCleanup(lambda: self.run_tool("monitor", "stop", check=False))
        self.run_tool("monitor", "start")
        status = self.run_tool("monitor", "status").stdout
        pid = int(re.search(r"pid=(\d+)", status)[1])
        os.kill(pid, signal.SIGSTOP)
        try:
            for index in range(int(limit.read_text()) + 128):
                (self.root / f"event-{index}").touch()
        finally:
            os.kill(pid, signal.SIGCONT)
        self.assertIn("fallback", self.run_tool("monitor", "status").stdout)
        self.makefile("all:\n\t@printf correct > result\n")
        self.run_tool("build", "--graph_optimizations=aggressive")
        self.assertEqual((self.root / "result").read_text(), "correct")

    def test_persistent_evaluation_handles_dynamic_state_and_effects(self):
        self.makefile("include config.mk\nNAME := X\nX = first\nX += $(EXTRA)\n"
                      "value = $(sort $($(NAME)))\n"
                      "define emit\n$(1) := $(2)\nendef\n"
                      "$(eval $(call emit,DYNAMIC,$(value)))\n"
                      "ifeq ($(SELECT),one)\nBRANCH := one\nelse\nBRANCH := two\nendif\n"
                      "WORDS := $(foreach x,b a,$(sort $(x) c))\n"
                      "$(info visible)\n$(file >>effects,once)\n"
                      ".PHONY: all\nall:\n"
                      "\t@printf '%s' '$(DYNAMIC)|$(BRANCH)|$(WORDS)' > result\n")
        for index, (extra, select) in enumerate((("z", "one"), ("z", "two"), ("a", "one"), ("a", "one"))):
            (self.root / "config.mk").write_text(f"EXTRA := {extra}\nSELECT := {select}\n")
            result = self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats")
            self.assertEqual(result.stdout.count("visible"), 1)
            expected = " ".join(sorted(("first", extra)))
            self.assertEqual((self.root / "result").read_text(), f"{expected}|{select}|b c a c")
            self.assertEqual((self.root / "effects").read_text().splitlines(), ["once"] * (index + 1))
        self.assertIn("evaluation regions:", result.stdout)
        self.assertGreater(int(re.search(r"evaluation regions: (\d+) hits", result.stdout)[1]), 0)
        cache = self.root / ".openmktr/.kati_stamp.evaluation"
        self.assertTrue(cache.exists())
        cache.write_bytes(b"broken")
        self.run_tool("build", "--graph_optimizations=aggressive")
        self.assertIn("first", (self.root / "result").read_text())

    def test_filesystem_queries_refresh_after_ordered_effects(self):
        source = ("BEFORE := $(wildcard *.marker)\n"
                  "FIND_BEFORE := $(shell find . -name '*.marker' -type f)\n"
                  "$(file >one.marker,one)\n"
                  "MIDDLE := $(wildcard *.marker)\n"
                  "FIND_MIDDLE := $(shell find . -name '*.marker' -type f)\n"
                  "DROP := $(shell rm -f one.marker)\n"
                  "AFTER := $(wildcard *.marker)\n"
                  "FIND_AFTER := $(shell find . -name '*.marker' -type f)\n"
                  ".PHONY: all\nall:\n"
                  "\t@printf '%s' '$(BEFORE)|$(FIND_BEFORE)|$(MIDDLE)|$(FIND_MIDDLE)|$(AFTER)|$(FIND_AFTER)' > result\n")
        for profile in ("off", "safe", "aggressive"):
            for mode in ([], ["build"]):
                self.makefile(source)
                self.run_tool(*mode, f"--graph_optimizations={profile}", "--use_find_emulator", "all")
                self.assertEqual((self.root / "result").read_text(), "||one.marker|./one.marker||")

    def test_include_iteration_keeps_its_snapshot_across_file_effects(self):
        for profile in ("off", "aggressive"):
            case = self.root / profile
            (case / "inc").mkdir(parents=True)
            (case / "Makefile").write_text("include inc/*.mk\nall:\n"
                                          "\t@printf '%s' '$(A)|$(B)|$(EXTRA)|$(SEEN)' > result\n")
            (case / "inc/a.mk").write_text("A := a\n$(file >inc/extra.mk,EXTRA := unexpected)\n"
                                           "SEEN := $(wildcard inc/*.mk)\n")
            (case / "inc/b.mk").write_text("B := b\n")
            subprocess.run([str(BINARY), "build", f"--graph_optimizations={profile}", "all"],
                           cwd=case, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True, timeout=30)
            self.assertEqual((case / "result").read_text(), "a|b||inc/a.mk inc/b.mk inc/extra.mk")

    def test_recursive_appends_invalidate_cached_expansions(self):
        self.makefile("X = one\nA := $(X)\nX += two\nB := $(X)\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(A)|$(B)' > result\n")
        for _ in range(2):
            self.run_tool("build", "--graph_optimizations=aggressive")
            self.assertEqual((self.root / "result").read_text(), "one|one two")

    def test_assignment_replay_preserves_state_precedence_and_branch_removal(self):
        (self.root / "config.mk").write_text("INPUT := two\n")
        source = ("include config.mk\nASSIGNED := $(sort $(INPUT) one)\n"
                  "COPY := $(ASSIGNED) tail\n"
                  "ifeq ($(KEEP),yes)\nREMOVABLE := present\nendif\n"
                  "$(info force evaluation)\n.PHONY: all\nall:\n"
                  "\t@printf '%s' '$(ASSIGNED)|$(COPY)|$(origin ASSIGNED)|$(REMOVABLE)' > result\n")
        self.makefile(source)
        options = ("build", "--graph_optimizations=aggressive", "--kati_stats", "KEEP=yes")
        self.run_tool(*options)
        second = self.run_tool(*options)
        self.assertGreater(int(re.search(r"assignment region replays: (\d+)", second.stdout)[1]), 0)
        self.assertEqual((self.root / "result").read_text(), "one two|one two tail|file|present")
        self.run_tool("build", "--graph_optimizations=aggressive", "KEEP=no", "ASSIGNED=override")
        self.assertEqual((self.root / "result").read_text(), "override|override tail|command line|")
        (self.root / "config.mk").write_text("INPUT := three\n")
        self.run_tool("build", "--graph_optimizations=aggressive", "KEEP=no")
        self.assertEqual((self.root / "result").read_text(), "one three|one three tail|file|")

    def test_undefined_read_replay_preserves_symbol_observations_and_invalidation(self):
        definitions = "".join(f"KEEP_{i} := value\nLOOK_{i} := dyn_missing_{i}\n" for i in range(200))
        expression = "".join(f"x$($(LOOK_{i}))" for i in reversed(range(200)))
        self.makefile("include config.mk\n" + definitions +
                      f"RESULT := {expression}x$($(zz_selector))\n" +
                      "$(info SYMBOLS=$(filter KEEP_%,$(.KATI_SYMBOLS)))\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(RESULT)' > result\n")
        for selector in ("dyn_missing_old", "dyn_missing_new"):
            (self.root / "config.mk").write_text(f"zz_selector := {selector}\n")
            ordinary = self.run_tool("build", "--graph_optimizations=off")
            expected = next(line for line in ordinary.stdout.splitlines() if line.startswith("SYMBOLS="))
            for _ in range(2):
                cached = self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats")
                observed = next(line for line in cached.stdout.splitlines() if line.startswith("SYMBOLS="))
                self.assertEqual(observed, expected)
                self.assertEqual((self.root / "result").read_text(), "x" * 201)
            self.assertGreater(int(re.search(r"assignment region replays: (\d+)", cached.stdout)[1]), 0)

    def test_pattern_scope_index_preserves_late_eval_and_append_order(self):
        source = ("FLAG := global\ndefine ADD_PATTERN\na%.o: FLAG += late\nendef\n"
                  ".SECONDEXPANSION:\n.PHONY: all trigger\n"
                  "all: trigger ab.o\ntrigger: $$(eval $$(ADD_PATTERN))\n"
                  "%.o: FLAG += broad\nab.o: FLAG += explicit\n"
                  "%.o:\n\t@printf '%s' '$(FLAG)' > $@\n")
        expected = None
        for profile in ("off", "safe", "aggressive"):
            for mode in ("direct", "ninja"):
                case = self.root / f"{profile}-{mode}"
                case.mkdir()
                (case / "Makefile").write_text(source)
                command = [str(BINARY)] + (["build"] if mode == "ninja" else [])
                subprocess.run([*command, f"--graph_optimizations={profile}", "all"],
                               cwd=case, check=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=30)
                value = (case / "ab.o").read_text()
                if expected is None:
                    expected = value
                self.assertEqual(value, expected)
                self.assertIn("late", value)

    def test_general_state_program_replays_mutations_in_fresh_invocations(self):
        source = ("include config.mk\n"
                  "fn = $(strip $(1))-$(SUFFIX)\n"
                  "LIST := $(foreach item,$(ITEMS),$(call fn,$(item)))\n"
                  "NAME := CHOICE\n$(NAME) = $(SUFFIX) deferred\nCHOICE += more\n"
                  "export CHOICE\nTEMP = obsolete\nundefine TEMP\n"
                  "vpath %.src source\n.SECONDEXPANSION:\n"
                  "ifeq ($(KEEP),yes)\nKEPT = present\nendif\n"
                  "define DECL\ndyn_$(TARGET):\n"
                  "\t@printf '%s' '$(TARGET)' > dynamic-result\nendef\n"
                  "$(eval $(DECL))\n.PHONY: all selected dyn_$(TARGET)\n"
                  "all: selected dyn_$(TARGET)\n"
                  "selected: export LOCAL := $(sort $(ITEMS))\n"
                  "selected: $$(AFTER)\n"
                  "\t@printf '%s' '$(LIST)|$(CHOICE)|$(origin CHOICE)|$(flavor CHOICE)|$(TEMP)|$(KEPT)|$(LOCAL)|$@' > result\n"
                  "\t@printf '%s' \"$$CHOICE|$$LOCAL\" > environment\n"
                  "AFTER = input.src\n$(file >>effects,x)\n")
        cases = []
        for mode, profile in (("direct", "off"), ("ninja", "off"),
                              ("ninja", "aggressive")):
            directory = self.root / f"{mode}-{profile}"
            directory.mkdir()
            (directory / "source").mkdir()
            (directory / "source/input.src").write_text("input\n")
            (directory / "Makefile").write_text(source)
            cases.append((directory, mode, profile))
        invocations = 0
        for suffix, items, keep, target in (("first", "c a", "yes", "one"),
                                            ("second", "d b", "no", "two")):
            for directory, _, _ in cases:
                (directory / "config.mk").write_text(
                    f"SUFFIX := {suffix}\nITEMS := {items}\nKEEP := {keep}\nTARGET := {target}\n")
            for repeat in range(2):
                invocations += 1
                expected = None
                for directory, mode, profile in cases:
                    command = [str(BINARY), *(["build"] if mode == "ninja" else []),
                               f"--graph_optimizations={profile}", "--kati_stats", "-j8", "all"]
                    result = subprocess.run(command, cwd=directory, check=True, text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            timeout=30)
                    outputs = [(directory / name).read_bytes()
                               for name in ("result", "environment", "dynamic-result")]
                    if expected is None:
                        expected = outputs
                    self.assertEqual(outputs, expected)
                    self.assertEqual((directory / "effects").read_text(), "x\n" * invocations)
                    if profile == "aggressive" and repeat:
                        for kind in ("assign", "rule", "conditional", "export",
                                     "undefine", "vpath", "command", "include"):
                            match = re.search(rf"state operation replays \({kind}\): (\d+)", result.stdout)
                            self.assertIsNotNone(match, result.stdout)
                            self.assertGreater(int(match[1]), 0)

    def test_call_foreach_reuse_preserves_local_symbol_observations(self):
        self.makefile("include config.mk\nfn = $(0):$(1):$(OUTER)\n"
                      "ifeq ($(FIRST),yes)\nEARLY := $(call fn,early)\nendif\n"
                      "RESULT := $(foreach $(ITER),$(ITEMS),$(call fn,$($(ITER))))\n"
                      "$(info LOCALS=$(sort $(filter 0 1 2 3 4 5 6 7 8 9 iter_%,$(.KATI_SYMBOLS))))\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(RESULT)' > result\n")
        for first, iterator, items in (("yes", "iter_old", "a b"),
                                       ("no", "iter_new", "c d")):
            (self.root / "config.mk").write_text(
                f"FIRST := {first}\nITER := {iterator}\nITEMS := {items}\nOUTER := global\n")
            ordinary = self.run_tool("build", "--graph_optimizations=off")
            expected = (self.root / "result").read_text()
            symbols = next(line for line in ordinary.stdout.splitlines() if line.startswith("LOCALS="))
            for _ in range(3):
                cached = self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats", "--regen_debug")
                self.assertEqual((self.root / "result").read_text(), expected)
                self.assertEqual(next(line for line in cached.stdout.splitlines()
                                      if line.startswith("LOCALS=")), symbols)
            self.assertGreater(int(re.search(r"assignment region replays: (\d+)", cached.stdout)[1]), 0)
            self.assertRegex(cached.stdout, r"state replay: Makefile:6 assign")

    def test_state_replay_reuses_downstream_status_without_repeating_effects(self):
        self.makefile("IGNORED := $(shell printf x >>effects; exit $(RET))\n"
                      "STATUS := $(.SHELLSTATUS)\n.PHONY: all\nall:\n"
                      "\t@printf '%s' '$(STATUS)' > result\n")
        invocations = 0
        for status in ("0", "1"):
            for repeat in range(2):
                invocations += 1
                result = self.run_tool("build", "--graph_optimizations=aggressive",
                                       "--kati_stats", "--regen_debug", f"RET={status}")
                self.assertEqual((self.root / "result").read_text(), status)
                self.assertEqual((self.root / "effects").read_text(), "x" * invocations)
                if repeat:
                    self.assertRegex(result.stdout, r"state replay: Makefile:2 assign")

    def test_eval_assignment_to_call_parameter_retains_the_live_binding(self):
        self.makefile("1 := outer\n"
                      "define inner\n$(eval 1 := discarded)$(1)\nendef\n"
                      ".PHONY: all\nall:\n"
                      "\t@printf '%s' '$(call inner,argument)|$(1)' > result\n")
        for profile in ("off", "safe", "aggressive"):
            for _ in range(2):
                self.run_tool("build", f"--graph_optimizations={profile}")
                self.assertEqual((self.root / "result").read_text(), "argument|outer")

    def test_native_file_and_wildcard_replay_tracks_negative_dependencies(self):
        (self.root / "files").mkdir()
        (self.root / "value").write_text("one\n")
        self.makefile("include config.mk\n"
                      "VALUE := $(file <value)|$(file <absent)\n"
                      "FILES := $(wildcard files/*.src files/new/*.src)\n"
                      "$(file >>effects,x)\n.PHONY: all\nall:\n"
                      "\t@printf '%s' '$(VALUE)|$(FILES)' > result\n")
        effects = 0
        for change in range(5):
            if change == 1:
                old = (self.root / "value").stat()
                (self.root / "value").write_text("two\n")
                os.utime(self.root / "value", ns=(old.st_atime_ns, old.st_mtime_ns))
            elif change == 2:
                (self.root / "absent").write_text("created\n")
                (self.root / "files/new").mkdir()
                (self.root / "files/new/a.src").write_text("source")
            elif change == 3:
                (self.root / "absent").unlink()
                (self.root / "files/new/a.src").unlink()
            (self.root / "config.mk").write_text(f"UNRELATED := {change}\n")
            expected = None
            for profile in ("off", "aggressive", "aggressive"):
                effects += 1
                result = self.run_tool("build", f"--graph_optimizations={profile}",
                                       "--regen_debug", "--kati_stats")
                value = (self.root / "result").read_text()
                if expected is None:
                    expected = value
                self.assertEqual(value, expected)
                self.assertEqual((self.root / "effects").read_text(), "x\n" * effects)
            self.assertRegex(result.stdout, r"state replay: Makefile:2 assign")
            self.assertRegex(result.stdout, r"state replay: Makefile:3 assign")
            self.assertRegex(result.stdout, r"native observations: [1-9]\d* validated")

    def test_native_find_replay_and_validation_track_search_membership(self):
        self.makefile("FAILED := $(shell exit 1)\n"
                      "FOUND := $(shell find . -name '*.src')\n"
                      "STATUS := $(.SHELLSTATUS)\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(FOUND)|$(STATUS)' > result\n")
        for change in range(4):
            if change == 1:
                before = self.root.stat()
                (self.root / "top.src").write_text("one")
                os.utime(self.root, ns=(before.st_atime_ns, before.st_mtime_ns))
            elif change == 2:
                (self.root / "new").mkdir()
                (self.root / "new/child.src").write_text("two")
            elif change == 3:
                (self.root / "top.src").unlink()
            expected = None
            for profile in ("off", "aggressive", "aggressive"):
                result = self.run_tool("build", f"--graph_optimizations={profile}",
                                       "--use_find_emulator", "--regen_debug", "--kati_stats")
                value = (self.root / "result").read_text()
                if expected is None:
                    expected = value
                self.assertEqual(value, expected)
                self.assertTrue(value.endswith("|0"))
            self.assertRegex(result.stdout, r"state replay: Makefile:2 assign")
        # Without arbitrary effects, the stamp checker must also find a new
        # top-level result even when the directory mtime was preserved.
        self.makefile("FOUND := $(shell find . -name '*.src')\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(FOUND)' > result\n")
        self.run_tool("build", "--use_find_emulator")
        before = self.root.stat()
        (self.root / "late.src").write_text("late")
        os.utime(self.root, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.run_tool("build", "--use_find_emulator")
        self.assertIn("late.src", (self.root / "result").read_text())

    def test_native_find_missing_parents_symlinks_and_diagnostics(self):
        self.makefile("FOUND := $(shell if [ -d absent ]; then find absent -name '*.src'; fi)\n"
                      "LINKED := $(shell find -L links -name '*.src')\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(FOUND)|$(LINKED)' > result\n")
        (self.root / "links").mkdir()
        (self.root / "one").mkdir()
        (self.root / "two").mkdir()
        (self.root / "one/first.src").write_text("one")
        (self.root / "two/second.src").write_text("two")
        (self.root / "links/target").symlink_to("../one")
        for change in range(3):
            if change == 1:
                (self.root / "absent").mkdir()
                (self.root / "absent/created.src").write_text("created")
                (self.root / "links/target").unlink()
                (self.root / "links/target").symlink_to("../two")
            elif change == 2:
                (self.root / "absent/created.src").unlink()
                (self.root / "absent").rmdir()
            expected = None
            for profile in ("off", "aggressive", "aggressive"):
                self.run_tool("build", "--use_find_emulator", f"--graph_optimizations={profile}")
                value = (self.root / "result").read_text()
                if expected is None:
                    expected = value
                self.assertEqual(value, expected)
        self.makefile("FOUND := $(shell find missing -name '*.src')\n"
                      ".PHONY: all\nall:\n\t@:\n")
        for _ in range(3):
            result = self.run_tool("build", "--use_find_emulator", "--graph_optimizations=aggressive")
            self.assertEqual(result.stdout.count("FindEmulator: find:"), 1, result.stdout)

    def test_recipe_metadata_epochs_reuse_pure_checks_and_invalidate_commands(self):
        self.makefile(".NOTPARALLEL:\n.PHONY: all FORCE mutate\n"
                      "all: first warm mutate second\n"
                      "first warm: seed FORCE\n"
                      "\t@$(if $(filter seed,$?),cp seed $@,:)\n"
                      "mutate: first warm\n"
                      "\t@printf new > seed; printf x >>effects\n"
                      "second: seed mutate FORCE\n"
                      "\t@$(if $(filter seed,$?),cp seed $@,:)\n")
        for profile in ("off", "safe", "aggressive"):
            old = time.time_ns() - 30_000_000_000
            (self.root / "seed").write_text("old")
            os.utime(self.root / "seed", ns=(old, old))
            for name in ("first", "warm", "second"):
                (self.root / name).write_text("settled")
                os.utime(self.root / name, ns=(old + 10_000_000_000,
                                              old + 10_000_000_000))
            (self.root / "effects").write_text("")
            result = self.run_tool(f"--graph_optimizations={profile}", "--kati_stats", "-j8")
            self.assertEqual((self.root / "second").read_text(), "new")
            self.assertEqual((self.root / "first").read_text(), "settled")
            self.assertEqual((self.root / "warm").read_text(), "settled")
            self.assertEqual((self.root / "effects").read_text(), "x")
            if profile != "off":
                self.assertRegex(result.stdout, r"recipe metadata probes: [1-9]\d* hits")

    def test_metadata_regions_overlapping_effects_use_ordinary_probes(self):
        self.makefile(".PHONY: all busy finish\nall: busy finish\n"
                      "busy:\n\t@printf new > input; touch ready; "
                      "while [ ! -f finished ]; do sleep .01; done\n"
                      "ready:\n\t@while [ ! -f ready ]; do sleep .01; done\n"
                      "check: ready input\n\t@printf '%s' '$(filter input,$?)' > seen\n"
                      "finish: check\n\t@touch finished\n")
        for enabled in (False, True):
            for name in ("ready", "finished", "seen"):
                (self.root / name).unlink(missing_ok=True)
            old = time.time_ns() - 30_000_000_000
            (self.root / "input").write_text("initial")
            os.utime(self.root / "input", ns=(old, old))
            (self.root / "check").write_text("settled")
            os.utime(self.root / "check", ns=(old + 10_000_000_000,
                                             old + 10_000_000_000))
            options = [] if enabled else ["--disable_graph_optimization=evaluation"]
            result = self.run_tool("-j2", "--kati_stats", *options)
            self.assertEqual((self.root / "input").read_text(), "new")
            self.assertEqual((self.root / "seen").read_text(), "input")
            if enabled:
                self.assertRegex(result.stdout, r"recipe metadata expansion bypasses: [1-9]\d*")

    def test_shell_effects_execute_once_on_invalidation(self):
        (self.root / "value").write_text("one")
        self.makefile("VALUE := $(shell printf x >> effects; cat value)\n"
                      ".PHONY: all\nall:\n\t@printf '%s' '$(sort $(VALUE))' > result\n")
        effects = 0
        for profile in ("safe", "aggressive"):
            for value in ("one", "two", "two"):
                (self.root / "value").write_text(value)
                self.run_tool("build", f"--graph_optimizations={profile}")
                effects += 1
                self.assertEqual((self.root / "effects").read_text(), "x" * effects)
                self.assertEqual((self.root / "result").read_text(), value)

    def test_file_reads_track_preserved_timestamps_and_empty_content(self):
        value = self.root / "value"
        value.write_text("one")
        self.makefile("VALUE := $(file <value)\n.PHONY: all\nall:\n"
                      "\t@printf '%s' '$(VALUE)' > result\n")
        for content in ("one", "two", ""):
            before = value.stat()
            value.write_text(content)
            os.utime(value, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.run_tool("build")
            self.assertEqual((self.root / "result").read_text(), content)

    def test_explicit_generated_include_certificate_invalidates(self):
        (self.root / "seed").write_text("MODE := one\n")
        self.makefile("include config.mk\n.PHONY: all\nall: result\n"
                      "config.mk: seed\n\t@cp seed config.mk\nresult: config.mk\n"
                      "\t@printf '%s' '$(MODE)' > result\n")
        self.run_tool("build")
        self.run_tool("build")
        graph = self.root / ".openmktr/build.ninja"
        before = graph.stat().st_mtime_ns
        self.run_tool("build")
        self.assertEqual(before, graph.stat().st_mtime_ns)
        original = (self.root / "seed").stat()
        (self.root / "seed").write_text("MODE := two\n")
        os.utime(self.root / "seed", ns=(original.st_atime_ns, original.st_mtime_ns))
        # Metadata changes invalidate the certificate even with a preserved mtime;
        # ordinary Make freshness still determines whether the include is remade.
        self.run_tool("build")
        self.assertNotEqual(before, graph.stat().st_mtime_ns)
        os.utime(self.root / "seed", None)
        self.run_tool("build")
        self.assertEqual((self.root / "result").read_text(), "two")

    def test_recursive_import_preserves_exports_and_working_directory(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text(".PHONY: all\nall: output\noutput: input\n"
                                        "\t@printf '%s' '$(LABEL):$(MAKELEVEL)' > output\n")
        (child / "input").write_text("input")
        self.makefile("export LABEL := inherited\n.PHONY: all child\nall: child\n"
                      "child:\n\t@$(MAKE) -C child all\n")
        result = self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats", "-j8")
        self.assertIn("imported child graph:", result.stdout)
        self.assertIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
        self.assertEqual((child / "output").read_text(), "inherited:1")
        before = (child / "output").stat().st_mtime_ns
        graph_before = (self.root / ".openmktr/build.ninja").stat().st_mtime_ns
        self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats", "-j8")
        self.assertEqual(before, (child / "output").stat().st_mtime_ns)
        self.assertEqual(graph_before, (self.root / ".openmktr/build.ninja").stat().st_mtime_ns)
        (child / "Makefile").write_text((child / "Makefile").read_text().replace("inherited", "different").replace("$(LABEL):", "changed:"))
        self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats", "-j8")
        # Make-style existing-output guards remain in effect; deleting the
        # output must execute the updated imported recipe.
        (child / "output").unlink()
        self.run_tool("build", "--graph_optimizations=aggressive", "--kati_stats", "-j8")
        self.assertEqual((child / "output").read_text(), "changed:1")

    def test_recursive_effectful_child_falls_back_without_discovery_effects(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text("$(file >>effects,once)\nall:\n\t@printf done > output\n")
        self.makefile(".PHONY: all\nall:\n\t@$(MAKE) -C child all\n")
        self.run_tool("build", "--graph_optimizations=aggressive")
        self.assertEqual((child / "effects").read_text().splitlines(), ["once"])
        self.assertNotIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
        self.makefile("export LABEL = $(shell pwd)\n.PHONY: all\nall: ready\n"
                      "\t@$(MAKE) -C child all\nready:\n\t@touch ready\n")
        (child / "Makefile").write_text("all: captured\ncaptured:\n"
                                        "\t@printf '%s' '$(LABEL)' > captured\n")
        self.run_tool("build", "--graph_optimizations=aggressive")
        self.assertNotIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
        self.assertEqual((child / "captured").read_text(), str(self.root))

    def test_mixed_recursive_children_keep_the_shared_budget_and_effects(self):
        self.makefile(".PHONY: all a b\nall: a b\na:\n\t@$(MAKE) -C a all\n"
                      "b:\n\t@$(MAKE) -C b all\n")
        for name in ("a", "b"):
            (self.root / name).mkdir()
        (self.root / "a/Makefile").write_text("all: output\noutput:\n\t@printf a > output\n")
        (self.root / "b/Makefile").write_text("$(file >>effects,once)\n"
                                              "all: output\noutput:\n\t@printf b > output\n")
        for index, jobs in enumerate((1, 8, 16), 1):
            self.run_tool("--ninja", "--graph_optimizations=aggressive", f"-j{jobs}")
            self.assertNotIn("subninja ", (self.root / "build.ninja").read_text())
            self.assertEqual((self.root / "b/effects").read_text() if index > 1 else "",
                             "once\n" * (index - 1))
            subprocess.run(["sh", "ninja.sh", f"-j{jobs}"], cwd=self.root, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual((self.root / "a/output").read_text(), "a")
            self.assertEqual((self.root / "b/output").read_text(), "b")
            self.assertEqual((self.root / "b/effects").read_text(), "once\n" * index)
        (self.root / "a/Makefile").write_text(".NOTPARALLEL:\nall: output\n"
                                              "output:\n\t@printf a > output\n")
        (self.root / "b/Makefile").write_text("all: output\noutput:\n\t@printf b > output\n")
        self.run_tool("build", "--graph_optimizations=aggressive", "-j16")
        self.assertNotIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())

    def test_recursive_import_propagates_feature_ablations(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text("all: output\noutput:\n\t@printf done > output\n")
        self.makefile(".PHONY: all\nall:\n\t@$(MAKE) -C child all\n")
        self.run_tool("build", "--graph_optimizations=aggressive",
                      "--disable_graph_optimization=compact")
        self.assertIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
        graphs = list({path.resolve() for path in (self.root / ".openmktr/.imports").rglob("build.ninja")})
        self.assertEqual(len(graphs), 1)
        self.assertNotIn("omktr_command", graphs[0].read_text())

    def test_recursive_discovery_does_not_duplicate_diagnostics(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text("all: output\noutput:\n\t@printf old > output\n"
                                        "output:\n\t@printf new > output\n")
        self.makefile(".PHONY: all\nall:\n\t@$(MAKE) -C child all\n")
        result = self.run_tool("build", "--graph_optimizations=aggressive")
        self.assertEqual(result.stdout.count("warning: overriding commands"), 1)
        self.assertNotIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
        self.assertEqual((child / "output").read_text(), "new")

    def test_recursive_import_retains_depfile_pool_and_wait_boundaries(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text("all: output\noutput:\n\t@printf done > output\n")
        for declaration, extra in (("", ["--detect_depfiles"]),
                                   ("all: .KATI_NINJA_POOL := console\n", []),
                                   ("all: MAKEFLAGS := -j2\n", []),
                                   ("all: MAKEFLAGS := -n\n", []),
                                   ("all: first .WAIT second\nfirst second:\n\t@:\n", [])):
            self.makefile(".PHONY: all first second\nall:\n\t@$(MAKE) -C child all\n" + declaration)
            self.run_tool("--ninja", "--graph_optimizations=aggressive", "-j8", *extra)
            self.assertNotIn("subninja ", (self.root / "build.ninja").read_text())
        self.makefile(".PHONY: all\nall:\n\t@$(MAKE) -C child all\n")
        for declaration in (".PRECIOUS: output\n", ".IGNORE: output\n"):
            (child / "Makefile").write_text("all: output\n" + declaration +
                                            "output:\n\t@printf done > output\n")
            self.run_tool("--ninja", "--graph_optimizations=aggressive")
            self.assertNotIn("subninja ", (self.root / "build.ninja").read_text())

    def test_recursive_import_rejects_shared_output_ownership(self):
        self.makefile(".PHONY: all a b\nall: a b\na:\n\t@$(MAKE) -C a all\n"
                      "b:\n\t@$(MAKE) -C b all\n")
        for name in ("a", "b"):
            child = self.root / name
            child.mkdir()
            (child / "Makefile").write_text(".PHONY: all\nall: ../shared\n"
                                            "../shared:\n\t@printf result > ../shared\n")
        self.run_tool("--ninja", "--graph_optimizations=aggressive")
        self.assertNotIn("subninja ", (self.root / "build.ninja").read_text())
        self.assertFalse((self.root / "shared").exists())
        subprocess.run(["sh", "ninja.sh", "-j8"], cwd=self.root, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual((self.root / "shared").read_text(), "result")

    def test_imported_children_wait_for_parent_prerequisites(self):
        self.makefile(".PHONY: all child\nall: child\nchild: ready\n"
                      "\t@$(MAKE) -C child all\nready:\n\t@printf ready > ready\n")
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text(".PHONY: all\nall: output\noutput:\n"
                                        "\t@test -f ../ready && printf done > output\n")
        for jobs in (1, 8, 16):
            self.run_tool("build", "--graph_optimizations=aggressive", f"-j{jobs}")
            self.assertEqual((child / "output").read_text(), "done")
            (self.root / "ready").unlink()
            (child / "output").unlink()

    def test_recursive_import_rejects_ambiguous_output_aliases(self):
        (self.root / "alias").symlink_to(".", target_is_directory=True)
        self.makefile(".PHONY: all a b\nall: a b\na:\n\t@$(MAKE) -C a all\n"
                      "b:\n\t@$(MAKE) -C b all\n")
        for name, output in (("a", "../shared"), ("b", "../alias/shared")):
            child = self.root / name
            child.mkdir()
            (child / "Makefile").write_text(f"all: {output}\n{output}:\n\t@printf value > {output}\n")
        self.run_tool("--ninja", "--graph_optimizations=aggressive")
        self.assertNotIn("subninja ", (self.root / "build.ninja").read_text())
        self.assertFalse((self.root / "shared").exists())

    def test_imported_recipe_failure_cleans_output(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text(".DELETE_ON_ERROR:\nall: output\noutput:\n"
                                        "\t@printf partial > output; exit 7\n")
        self.makefile(".PHONY: all\nall:\n\t@$(MAKE) -C child all\n")
        failed = self.run_tool("build", "--graph_optimizations=aggressive", "-j8", check=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
        self.assertFalse((child / "output").exists())

    def test_imported_recipe_interruption_cleans_output_and_releases_budget(self):
        child = self.root / "child"
        child.mkdir()
        (child / "Makefile").write_text("all: output\noutput:\n"
                                        "\t@printf partial > output; touch ready; sleep 30\n")
        self.makefile(".PHONY: all\nall:\n\t@$(MAKE) -C child all\n")
        command = [str(BINARY), "build", "--graph_optimizations=aggressive", "-j16"]
        process = subprocess.Popen(command, cwd=self.root, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not (child / "ready").exists() and process.poll() is None:
                if time.monotonic() > deadline:
                    self.fail("imported recipe did not start")
                time.sleep(0.01)
            process.send_signal(signal.SIGINT)
            process.communicate(timeout=10)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("subninja ", (self.root / ".openmktr/build.ninja").read_text())
            self.assertFalse((child / "output").exists())
            (child / "Makefile").write_text("all: output\noutput:\n\t@printf recovered > output\n")
            self.run_tool(*command[1:])
            self.assertEqual((child / "output").read_text(), "recovered")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)

    def test_include_proof_corruption_is_a_cache_miss(self):
        (self.root / "seed").write_text("MODE := one\n")
        self.makefile("include config.mk\nall: config.mk\nconfig.mk: seed\n\t@cp seed config.mk\n")
        self.run_tool("build")
        self.run_tool("build")
        proof = self.root / ".openmktr/.kati_stamp.includes"
        self.assertTrue(proof.exists())
        proof.write_bytes(b"corrupt")
        self.run_tool("build")
        self.assertNotEqual(proof.read_bytes(), b"corrupt")

    def test_monitor_lifecycle_and_changed_input(self):
        self.addCleanup(lambda: self.run_tool("monitor", "stop", check=False))
        self.makefile("all: output\noutput: input\n\t@cp input output\n")
        (self.root / "input").write_text("one")
        self.assertIn("ready", self.run_tool("monitor", "start").stdout)
        self.assertIn("ready", self.run_tool("monitor", "status").stdout)
        self.run_tool("build", "--graph_optimizations=aggressive")
        self.run_tool("build", "--graph_optimizations=aggressive", "--regen_debug")
        (self.root / "input").write_text("two")
        self.run_tool("build", "--graph_optimizations=aggressive")
        self.assertEqual((self.root / "output").read_text(), "two")
        self.run_tool("monitor", "stop")
        self.assertNotEqual(self.run_tool("monitor", "status", check=False).returncode, 0)
        self.run_tool("build", "--graph_optimizations=aggressive")

    def test_corrupt_evaluation_records_fall_back_to_ordinary_evaluation(self):
        self.makefile("VALUE := $(sort z b a z)\nall:\n\t@printf '$(VALUE)' > result\n")
        for damaged in (b"corrupt", b"OMKTR_GRAPH0", b"OMKTR_GRAPH1" + b"\xff" * 32):
            self.run_tool("--ninja", "--graph_optimizations=aggressive")
            cache = self.root / ".kati_stamp.evaluation"
            self.assertTrue(cache.is_file())
            cache.write_bytes(damaged)
            self.run_tool("--ninja", "--graph_optimizations=aggressive")
            self.assertNotEqual(cache.read_bytes(), damaged)
            subprocess.run(["sh", "ninja.sh"], cwd=self.root, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual((self.root / "result").read_text(), "a b z")

    def test_missing_native_read_does_not_hide_a_later_path_error(self):
        for profile in ("off", "safe", "aggressive"):
            for force_evaluation in (False, True):
                slot = self.root / "slot"
                if slot.exists():
                    slot.unlink()
                self.makefile("VALUE := $(file <slot/value)\n" +
                              ("$(file >>effects,x)\n" if force_evaluation else "") +
                              ".PHONY: all\nall:\n\t@printf '%s' '$(VALUE)' > result\n")
                self.run_tool("build", f"--graph_optimizations={profile}")
                slot.write_text("not a directory")
                ordinary = self.run_tool("--graph_optimizations=off", check=False)
                cached = self.run_tool("build", f"--graph_optimizations={profile}", check=False)
                self.assertEqual(cached.returncode, ordinary.returncode)
                self.assertNotEqual(cached.returncode, 0)
                self.assertIn("*** open failed", cached.stdout)

    def test_stamp_integrity_rejects_flag_corruption_and_tampered_graphs(self):
        self.makefile("$(file >>effects,x)\n.PHONY: all\nall:\n\t@printf correct > result\n")
        self.run_tool("build")
        stamp = self.root / ".openmktr/.kati_stamp"
        original = stamp.read_bytes()
        damaged = bytearray(original)
        damaged[12:16] = bytes(4)
        stamp.write_bytes(damaged)
        self.run_tool("build")
        self.assertEqual((self.root / "effects").read_text(), "x\nx\n")
        self.assertNotEqual(stamp.read_bytes(), damaged)
        self.makefile(".PHONY: all\nall:\n\t@printf correct > result\n")
        self.run_tool("build")
        graph = self.root / ".openmktr/build.ninja"
        graph.write_text(graph.read_text().replace("printf correct", "printf wrong"))
        self.run_tool("build")
        self.assertEqual((self.root / "result").read_text(), "correct")
        dumped = self.run_tool("--dump_stamp_tool", "--files", str(stamp))
        self.assertIn("Makefile", dumped.stdout)
        self.assertNotIn(".openmktr/build.ninja", dumped.stdout)
        # The mutable checkpoint is deliberately excluded from the structural
        # seal; an ordinary no-op refresh must retain a valid stamp.
        self.run_tool("build")
        self.run_tool("--dump_stamp_tool", "--files", str(stamp))

    def test_nonregular_cache_files_are_nonblocking_misses(self):
        self.makefile("VALUE := $(sort c b a)\n.PHONY: all\nall:\n"
                      "\t@printf '%s' '$(VALUE)' > result\n")
        self.run_tool("--ninja", "--graph_optimizations=aggressive")
        cache = self.root / ".kati_stamp.evaluation"
        cache.unlink()
        os.mkfifo(cache)
        subprocess.run([str(BINARY), "--ninja", "--graph_optimizations=aggressive"],
                       cwd=self.root, check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=5)
        self.assertTrue(cache.is_file())
        self.run_tool("build")
        stamp = self.root / ".openmktr/.kati_stamp"
        stamp.unlink()
        os.mkfifo(stamp)
        subprocess.run([str(BINARY), "build"], cwd=self.root, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
        self.assertTrue(stamp.is_file())
        self.assertEqual((self.root / "result").read_text(), "a b c")
        stamp.unlink()
        stamp.mkdir()
        (stamp / "sentinel").write_text("preserved")
        self.run_tool("build")
        self.assertEqual((stamp / "sentinel").read_text(), "preserved")
        (stamp / "sentinel").unlink()
        stamp.rmdir()
        sentinel = self.root / "sentinel"
        sentinel.write_text("preserved")
        stamp.symlink_to(sentinel)
        self.run_tool("build")
        self.assertEqual(sentinel.read_text(), "preserved")
        self.assertNotEqual(stamp.resolve(), sentinel)
        self.assertIn(".omktr-generations", str(stamp.resolve()))

    def test_monitor_tracks_include_rename_delete_recreate_and_symlinks(self):
        if not Path("/dev/shm").is_dir():
            self.skipTest("Linux tmpfs unavailable")
        with tempfile.TemporaryDirectory(prefix="omktr-monitor-paths-", dir="/dev/shm") as temp:
            original_root, self.root = self.root, Path(temp)
            try:
                self.makefile("-include config/value.mk\nVALUE ?= absent\n"
                              ".PHONY: all\nall:\n\t@printf '$(VALUE)' > result\n")
                directory = self.root / "config"
                directory.mkdir()
                include = directory / "value.mk"
                include.write_text("VALUE := one\n")
                self.run_tool("monitor", "start")

                def check(value):
                    self.run_tool("build", "--graph_optimizations=aggressive")
                    self.assertEqual((self.root / "result").read_text(), value)

                check("one")
                include.rename(directory / "previous.mk")
                include.write_text("VALUE := two\n")
                check("two")
                include.unlink()
                check("absent")
                include.write_text("VALUE := three\n")
                check("three")
                for value in ("four", "five"):
                    target = self.root / (value + ".mk")
                    target.write_text(f"VALUE := {value}\n")
                    include.unlink()
                    include.symlink_to(target)
                    check(value)
                directory.rename(self.root / "previous-directory")
                directory.mkdir()
                include.write_text("VALUE := six\n")
                check("six")
            finally:
                self.run_tool("monitor", "stop", check=False)
                self.root = original_root

    def test_monitor_checkpoints_cover_validation_and_restart(self):
        # tmpfs exercises monitor acceleration even when the container's normal
        # temporary directory uses an unsupported overlay filesystem.
        if not Path("/dev/shm").is_dir():
            self.skipTest("Linux tmpfs unavailable")
        with tempfile.TemporaryDirectory(prefix="omktr-checkpoint-", dir="/dev/shm") as temp:
            original_root, self.root = self.root, Path(temp)
            try:
                self.makefile("all:\n\t@:\n")
                self.run_tool("monitor", "start")

                def request(payload):
                    with socket.socket(socket.AF_UNIX) as client:
                        client.settimeout(5)
                        client.connect(str(self.root / ".openmktr-monitor.sock"))
                        client.sendall(struct.pack("=I", len(payload)) + payload)

                        def receive(length):
                            result = b""
                            while len(result) < length:
                                part = client.recv(length - len(result))
                                if not part:
                                    raise AssertionError("incomplete monitor response")
                                result += part
                            return result

                        return receive(struct.unpack("=I", receive(4))[0])

                path = b"Makefile"
                snapshot = request(struct.pack("<Q", len(path)) + path)
                length = struct.unpack("<Q", snapshot[:8])[0]
                self.assertEqual(snapshot[8:8 + length], b"snapshot2")
                snapshot = snapshot[8 + length:]
                length = struct.unpack("<Q", snapshot[:8])[0]
                checkpoint = b"checkpoint:" + snapshot[8:8 + length]
                self.assertEqual(request(checkpoint), b"stable")
                (self.root / "new-directory").mkdir()
                self.assertEqual(request(checkpoint), b"fallback")
                self.run_tool("monitor", "stop")
                self.run_tool("monitor", "start")
                self.assertEqual(request(checkpoint), b"fallback")
            finally:
                self.run_tool("monitor", "stop", check=False)
                self.root = original_root

    def test_ninja_fifo_creation_reports_errors_and_preserves_collisions(self):
        self.makefile(".PHONY: all\nall:\n\t@:\n")
        self.run_tool("--ninja", "-j1")
        tools = self.root / "tools"
        tools.mkdir()
        temporary = self.root / "temporary"
        temporary.mkdir()
        shim = tools / "mkfifo"
        shim.write_text("#!/bin/sh\nprintf 'deliberate FIFO setup failure\\n' >&2\nexit 1\n")
        shim.chmod(0o755)
        env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                   TMPDIR=str(temporary), KATI_JOBS="1")
        env.pop("KATI_JOBSERVER_FIFO", None)

        def launch():
            child = subprocess.Popen(["sh", "./ninja.sh", "-j1"], cwd=self.root,
                                     env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, start_new_session=True)
            try:
                output, _ = child.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                output, _ = child.communicate()
                self.fail("FIFO creation did not terminate: " + output)
            return child.returncode, output

        status, output = launch()
        self.assertNotEqual(status, 0)
        self.assertIn("cannot create jobserver FIFO", output)
        self.assertIn("deliberate FIFO setup failure", output)
        self.assertEqual(list(temporary.iterdir()), [])
        shim.write_text(f"#!{sys.executable}\n"
                        "from pathlib import Path\nimport os,sys\n"
                        "marker=Path('fifo-collision')\n"
                        "path=Path(sys.argv[-1])\n"
                        "if not marker.exists():\n"
                        " path.symlink_to('missing-collision-target')\n"
                        " marker.write_text(str(path))\n"
                        " sys.exit(1)\n"
                        "os.mkfifo(path,0o600)\n")
        status, output = launch()
        self.assertEqual(status, 0, output)
        collision = Path((self.root / "fifo-collision").read_text())
        self.assertTrue(collision.is_symlink())
        self.assertEqual(os.readlink(collision), "missing-collision-target")
        self.assertEqual(list(temporary.iterdir()), [collision])

    def test_opaque_recursion_inherits_profiles_and_feature_ablations(self):
        (self.root / "child/grand").mkdir(parents=True)
        self.makefile(".EXPORT_ALL_VARIABLES:\n"
                      "export KATI_GRAPH_SETTINGS := invalid-make-export\n"
                      "all:\n\t@$(MAKE) --kati_stats -C child all\n")
        (self.root / "child/Makefile").write_text(
            ".NOTPARALLEL:\nall:\n\t@$(MAKE) --kati_stats -C grand all\n")
        (self.root / "child/grand/Makefile").write_text(
            "all:\n\t@printf '%s\\n' \"$$KATI_GRAPH_SETTINGS\" "
            "'$(origin KATI_GRAPH_SETTINGS)' '$(MAKE)' > result\n")
        cases = [
            (["--graph_optimizations=off"], "v1:off",
             "evaluation=0 includes=0 recursive=0 compact=0 monitor=0 full=0 pattern_index=0 resolution=0"),
            (["--graph_optimizations=safe"], "v1:safe",
             "evaluation=1 includes=1 recursive=0 compact=1 monitor=0 full=0 pattern_index=1 resolution=1"),
            (["--graph_optimizations=aggressive", "--disable_graph_optimization=evaluation",
              "--disable_graph_optimization=compact"], "v1:aggressive:evaluation:compact",
             "evaluation=0 includes=1 recursive=1 compact=0 monitor=1 full=1 pattern_index=1 resolution=1"),
            (["--graph_optimizations=aggressive", "--disable_graph_optimization=pattern_index"],
             "v1:aggressive:pattern_index",
             "evaluation=1 includes=1 recursive=1 compact=1 monitor=1 full=1 pattern_index=0 resolution=1"),
        ]
        cases.append((["--graph_optimizations=safe", "--disable_graph_optimization=resolution"],
                      "v1:safe:resolution",
                      "evaluation=1 includes=1 recursive=0 compact=1 monitor=0 full=0 pattern_index=1 resolution=0"))
        for jobs in (1, 8, 16):
            for options, transport, settings in cases:
                for ninja in (False, True):
                    with self.subTest(jobs=jobs, options=options, ninja=ninja):
                        arguments = ["--kati_stats", f"-j{jobs}", *options, "all"]
                        if ninja:
                            output = self.run_tool("--ninja", "--regen", *arguments).stdout
                            output += subprocess.run(
                                ["sh", "./ninja.sh", f"-j{jobs}"], cwd=self.root,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=30, check=True).stdout
                        else:
                            output = self.run_tool(*arguments).stdout
                        observations = [line for line in output.splitlines()
                                        if "graph optimization settings:" in line]
                        self.assertEqual(len(observations), 3, output)
                        self.assertTrue(all(line.endswith(settings) for line in observations), output)
                        self.assertEqual((self.root / "child/grand/result").read_text().splitlines(),
                                         [transport, "undefined", str(BINARY)])

    def test_invalid_inherited_graph_controls_fall_back_and_cli_profile_wins(self):
        self.makefile("all:\n\t@:\n")
        env = dict(os.environ, KATI_GRAPH_SETTINGS="v99:aggressive:unknown")
        output = self.run_tool("--kati_stats", env=env).stdout
        self.assertIn("ignored invalid inherited graph controls", output)
        self.assertIn("evaluation=0 includes=0 recursive=0 compact=0 monitor=0 full=0", output)
        output = self.run_tool("--kati_stats", "--graph_optimizations=aggressive", env=env).stdout
        self.assertIn("evaluation=1 includes=1 recursive=1 compact=1 monitor=1 full=1", output)

    def test_profile_and_ablation_validation(self):
        self.makefile("all:\n\t@:\n")
        for option in ("--graph_optimizations=unknown", "--disable_graph_optimization=unknown"):
            self.assertNotEqual(self.run_tool(option, check=False).returncode, 0)
        result = self.run_tool("build", "--graph_optimizations=aggressive",
                               "--disable_graph_optimization=compact", "--kati_stats")
        self.assertIn("graph validation and generation time:", result.stdout)
        self.assertIn("Ninja execution time:", result.stdout)
        self.assertNotIn("omktr_command", (self.root / ".openmktr/build.ninja").read_text())


if __name__ == "__main__":
    unittest.main()
