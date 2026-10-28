#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise Ninja >= 1.13.2's GNU FIFO client beside Openmktr's scheduler.

The reference FIFO is provided by Python; no GNU Make dependency is needed.
"""

import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from correctness import KATI


class NinjaJobserver(unittest.TestCase):
    def test_idle_inherited_reservation_wakes_waiting_recipes(self):
        with tempfile.TemporaryDirectory(prefix="omktr-reserved-wakeup-") as temp:
            cwd = Path(temp)
            fifo = cwd / "fifo"
            os.mkfifo(fifo)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            self.addCleanup(os.close, fd)
            # All FIFO tokens are owned elsewhere. This process still owns
            # one inherited slot, which must be reusable by queued recipes.
            env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_")
                   and k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
            env.update(KATI_JOBSERVER_FIFO=str(fifo), KATI_JOBSERVER_RESERVED="1")
            (cwd / "counts").write_text("[0, 0, 0]")
            (cwd / "worker.py").write_text(
                "import fcntl,json,sys,time\nfrom pathlib import Path\n"
                "def update(delta):\n"
                " with open('counts','r+') as stream:\n"
                "  fcntl.flock(stream,fcntl.LOCK_EX); a,p,n=json.load(stream)\n"
                "  a+=delta; stream.seek(0); json.dump([a,max(p,a),n+(delta<0)],stream); stream.truncate()\n"
                "update(1); time.sleep(.05); Path(sys.argv[1]).write_text('built'); update(-1)\n")
            (cwd / "Makefile").write_text(
                "all: a b c d\na b c d:\n\t@python3 worker.py $@\n")
            child = subprocess.Popen([str(KATI), "-j4", "all"], cwd=cwd, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, start_new_session=True)
            try:
                try:
                    output, _ = child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    child.send_signal(signal.SIGTERM)
                    child.communicate(timeout=5)
                    self.fail("waiting recipes left the inherited reservation idle")
                self.assertEqual(child.returncode, 0, output)
                self.assertEqual(json.loads((cwd / "counts").read_text()), [0, 1, 4])
                for name in "abcd":
                    self.assertEqual((cwd / name).read_text(), "built")
                with self.assertRaises(BlockingIOError):
                    os.read(fd, 16)  # Reservation notifications are not FIFO capacity.
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate()

    @unittest.skipUnless(sys.platform == "linux", "Linux descriptor inheritance")
    def test_parallel_recipes_do_not_inherit_sibling_output_pipes(self):
        with tempfile.TemporaryDirectory(prefix="omktr-pipe-inheritance-") as temp:
            cwd = Path(temp)
            # Widen the old pipe()+fcntl() race without changing pipe semantics.
            # A sibling spawn during this delay must not inherit these ends.
            (cwd / "slow_pipe.c").write_text(
                "#include <unistd.h>\n#include <sys/syscall.h>\n"
                "int pipe(int fd[2]) {\n"
                " int result = syscall(SYS_pipe2, fd, 0);\n"
                " if (!result) usleep(50000);\n return result;\n}\n")
            subprocess.run(["clang", "-shared", "-fPIC", "slow_pipe.c", "-o", "slow_pipe.so"],
                           cwd=cwd, check=True, capture_output=True, timeout=30)
            (cwd / "worker.py").write_text(
                "import os,sys\nfrom pathlib import Path\n"
                "leaked=[]\n"
                "for fd in os.listdir('/proc/self/fd'):\n"
                " if int(fd)<=2: continue\n"
                " try: target=os.readlink('/proc/self/fd/'+fd)\n"
                " except FileNotFoundError: continue\n"
                " if target.startswith('pipe:'): leaked.append((fd,target))\n"
                "assert not leaked, leaked\nPath(sys.argv[1]).write_text('clean')\n")
            (cwd / "Makefile").write_text(
                "all: " + " ".join(f"out{i}" for i in range(64)) + "\n" +
                "".join(f"out{i}:\n\t@python3 worker.py $@\n" for i in range(64)))
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith("KATI_") and k not in ("MAKEFLAGS", "MAKEOVERRIDES")}
            env["LD_PRELOAD"] = str(cwd / "slow_pipe.so")
            result = subprocess.run([str(KATI), "-j16", "all"], cwd=cwd, env=env,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for i in range(64):
                self.assertEqual((cwd / f"out{i}").read_text(), "clean")

    def test_repeated_closed_stdout_commands_do_not_leak_process_descriptors(self):
        with tempfile.TemporaryDirectory(prefix="omktr-process-fds-") as temp:
            cwd = Path(temp)
            (cwd / "Makefile").write_text(
                "".join(f"V{i} := $(shell printf x; exec 1>&-; sleep .001)\n" for i in range(128)) +
                "all:\n\t@printf '%s' '" + "".join(f"$(V{i})" for i in range(128)) + "' > result\n")
            for mode in ([], ["build"]):
                result = subprocess.run([str(KATI), *mode, "all"], cwd=cwd,
                                        capture_output=True, text=True, timeout=15,
                                        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64)))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((cwd / "result").read_text(), "x" * 128)

    def test_closed_stdout_preserves_exit_status_and_cancellation(self):
        with tempfile.TemporaryDirectory(prefix="omktr-closed-stdout-") as temp:
            cwd = Path(temp)
            env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_")}
            result = subprocess.run([str(KATI), "--internal-run-recipe", "/bin/sh", "-c",
                                     "exec 1>&-; sleep .02; exit 23"], cwd=cwd, env=env,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 23, result.stderr)
            child = subprocess.Popen(
                [str(KATI), "--internal-run-recipe", "/bin/sh", "-c",
                 "exec 1>&-; trap 'printf stopped > stopped; exit 0' TERM; "
                 "printf ready > ready; while :; do sleep .1; done"], cwd=cwd, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            try:
                deadline = time.monotonic() + 10
                while not (cwd / "ready").exists() and child.poll() is None:
                    if time.monotonic() > deadline:
                        self.fail("recipe did not reach closed-stdout wait")
                    time.sleep(.01)
                self.assertTrue((cwd / "ready").exists())
                child.send_signal(signal.SIGTERM)
                output, error = child.communicate(timeout=10)
                self.assertEqual(child.returncode, 128 + signal.SIGTERM, output + error)
                self.assertEqual((cwd / "stopped").read_text(), "stopped")
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate()

    def test_managed_build_inherits_fifo_and_preserves_explicit_job_overrides(self):
        with tempfile.TemporaryDirectory(prefix="omktr-managed-fifo-") as temp:
            cwd = Path(temp)
            fifo = cwd / "fifo"
            os.mkfifo(fifo)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            self.addCleanup(os.close, fd)
            os.write(fd, b"+")
            second_fifo = cwd / "second-fifo"
            os.mkfifo(second_fifo)
            second_fd = os.open(second_fifo, os.O_RDWR | os.O_NONBLOCK)
            self.addCleanup(os.close, second_fd)
            os.write(second_fd, b"++")
            env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_")
                   and k not in ("MAKEFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
            # The available token quota deliberately differs from the advertised -j.
            env["MAKEFLAGS"] = f" -j8 --jobserver-auth=fifo:{fifo}"
            (cwd / "worker.py").write_text(
                "import fcntl, json, time\n"
                "def update(delta):\n"
                "    with open('counts', 'r+') as f:\n"
                "        fcntl.flock(f, fcntl.LOCK_EX)\n"
                "        active, peak = json.load(f)\n"
                "        active += delta\n"
                "        f.seek(0)\n"
                "        json.dump([active, max(peak, active)], f)\n"
                "        f.truncate()\n"
                "update(1)\ntime.sleep(.15)\nupdate(-1)\n")
            (cwd / "Makefile").write_text(
                ".PHONY: all a b c d e f g h\nall: a b c d e f g h\n"
                "a b c d e f g h:\n\t@python3 worker.py\n")
            for flags, expected_peak, auth in (([], 2, fifo), ([], 3, second_fifo),
                                               (["-j1"], 1, fifo), (["-j", "3"], 3, fifo),
                                               (["-kj3"], 3, fifo), (["-n"], 0, fifo)):
                with self.subTest(flags=flags):
                    env["MAKEFLAGS"] = f" -j8 --jobserver-auth=fifo:{auth}"
                    (cwd / "counts").write_text("[0, 0]")
                    child = subprocess.Popen([str(KATI), "build", *flags, "all"], cwd=cwd,
                                             env=env, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True,
                                             start_new_session=True)
                    try:
                        output, _ = child.communicate(timeout=15)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.communicate()
                        self.fail("managed FIFO client deadlocked")
                    self.assertEqual(child.returncode, 0, output)
                    self.assertEqual(json.loads((cwd / "counts").read_text()), [0, expected_peak])
                    for descriptor, tokens in ((fd, b"+"), (second_fd, b"++")):
                        self.assertEqual(os.read(descriptor, 16), tokens, "managed build leaked FIFO tokens")
                        os.write(descriptor, tokens)

    def test_interrupt_returns_fifo_tokens_and_allows_recovery(self):
        with tempfile.TemporaryDirectory(prefix="omktr-ninja-interrupt-") as temp:
            cwd = Path(temp)
            fifo = cwd / "fifo"
            os.mkfifo(fifo)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            self.addCleanup(os.close, fd)
            os.write(fd, b"+")
            env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_")
                   and k not in ("MAKEFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
            env["MAKEFLAGS"] = f" -j2 --jobserver-auth=fifo:{fifo}"
            (cwd / "worker.py").write_text(
                "from pathlib import Path\nimport time\n"
                "Path('started').touch()\ntime.sleep(60)\n")
            (cwd / "Makefile").write_text(
                ".PHONY: all a b\nall: a b\na b:\n\t@python3 worker.py\n")
            child = subprocess.Popen([str(KATI), "build", "all"], cwd=cwd, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, start_new_session=True)
            try:
                deadline = time.monotonic() + 10
                while not (cwd / "started").exists() and child.poll() is None:
                    if time.monotonic() >= deadline:
                        self.fail("Ninja did not start its worker")
                    time.sleep(0.02)
                self.assertTrue((cwd / "started").exists())
                os.killpg(child.pid, signal.SIGINT)
                output, _ = child.communicate(timeout=10)
                self.assertEqual(child.returncode, 128 + signal.SIGINT, output)
                self.assertEqual(os.read(fd, 16), b"+", "interrupt leaked a jobserver token")
                os.write(fd, b"+")
                (cwd / "worker.py").write_text("print('recovered')\n")
                recovered = subprocess.run([str(KATI), "build", "all"], cwd=cwd, env=env,
                                           capture_output=True, text=True, timeout=10)
                self.assertEqual(recovered.returncode, 0, recovered.stdout + recovered.stderr)
                self.assertIn("recovered", recovered.stdout)
                self.assertIn("ninja_required_version = 1.13.2",
                              (cwd / ".openmktr/build.ninja").read_text())
                self.assertEqual(os.read(fd, 16), b"+")
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate()

    def test_fifo_client_and_recursive_launcher(self):
        version = subprocess.check_output(["ninja", "--version"], text=True).strip()
        self.assertGreaterEqual(tuple(map(int, re.match(r"(\d+)\.(\d+)\.(\d+)", version).groups())),
                                (1, 13, 2), version)
        with tempfile.TemporaryDirectory(prefix="omktr-ninja-jobserver-") as temp:
            cwd = Path(temp)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith("KATI_") and k not in
                   ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
            descriptors = []
            for name, tokens in (("gnu-fifo", b"+"), ("kati-fifo", b"xx")):
                fifo = cwd / name
                os.mkfifo(fifo)
                fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
                descriptors.append(fd)
                self.addCleanup(os.close, fd)
                os.write(fd, tokens)
            env.update(MAKEFLAGS=f" -j2 --jobserver-auth=fifo:{cwd / 'gnu-fifo'}",
                       KATI_JOBSERVER_FIFO=str(cwd / "kati-fifo"))
            (cwd / "worker.py").write_text(
                "import fcntl, json, sys, time\n"
                "def update(delta):\n"
                "    with open('counts', 'r+') as f:\n"
                "        fcntl.flock(f, fcntl.LOCK_EX)\n"
                "        active, peak, done = json.load(f)\n"
                "        active += delta\n"
                "        f.seek(0)\n"
                "        json.dump([active, max(peak, active), done + (delta < 0)], f)\n"
                "        f.truncate()\n"
                "update(1)\ntime.sleep(0.15)\nupdate(-1)\n"
                "sys.exit(1 if len(sys.argv) > 1 else 0)\n")
            # Establish that the installed Ninja really uses the advertised FIFO.
            (cwd / "control.ninja").write_text(
                "rule work\n  command = python3 worker.py\n" +
                "".join(f"build control{i}: work\n" for i in range(8)))
            (cwd / "Makefile").write_text(
                f"export MAKEFLAGS := {env['MAKEFLAGS']}\n"
                ".PHONY: all a b c d\nall: a b c d\na b c d:\n"
                '\t@"$(MAKE)" -j1 -f child.mk all\n')
            (cwd / "child.mk").write_text(
                ".PHONY: all x y\nall: x y\nx y:\n\t@python3 worker.py $(FAIL)\n")

            def run(command, success=True):
                # Kill descendants as well if either scheduler deadlocks.
                child = subprocess.Popen(command, cwd=cwd, env=env,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, start_new_session=True)
                try:
                    output, _ = child.communicate(timeout=30)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate()
                    self.fail(f"jobserver deadlock: {command}")
                self.assertEqual(child.returncode == 0, success, output)
                return output

            def check(command, jobs, done, success=True, expected_peak=None):
                (cwd / "counts").write_text("[0, 0, 0]")
                run(command, success)
                active, peak, completed = json.loads((cwd / "counts").read_text())
                self.assertEqual(active, 0)
                self.assertGreater(peak, 0)
                self.assertLessEqual(peak, jobs)
                if success:
                    self.assertEqual(completed, done)
                    if expected_peak is not None:
                        self.assertEqual(peak, expected_peak)
                for fd, tokens in zip(descriptors, (b"+", b"xx")):
                    self.assertEqual(os.read(fd, 16), tokens, "jobserver token leak")
                    os.write(fd, tokens)

            check(["ninja", "-f", "control.ninja"], 2, 8, expected_peak=2)
            for fail in (False, True):
                run([str(KATI), "--ninja", "all", f"FAIL={'fail' if fail else ''}"])
                self.assertIn(f"--jobserver-auth=fifo:{cwd / 'gnu-fifo'}",
                              (cwd / "env.sh").read_text())
                for flags, jobs in (([], 2), (["-j1"], 1)):
                    with self.subTest(fail=fail, flags=flags):
                        check(["sh", "./ninja.sh", *flags], jobs, 8, not fail)


if __name__ == "__main__":
    unittest.main()
