#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise Ninja >= 1.13's GNU FIFO client beside Openmktr's scheduler.

The reference FIFO is provided by Python; no GNU Make dependency is needed.
"""

import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import unittest

from correctness import KATI


class NinjaJobserver(unittest.TestCase):
    def test_fifo_client_and_recursive_launcher(self):
        version = subprocess.check_output(["ninja", "--version"], text=True).strip()
        self.assertGreaterEqual(tuple(map(int, re.match(r"(\d+)\.(\d+)", version).groups())),
                                (1, 13), version)
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
