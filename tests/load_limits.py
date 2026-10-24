#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Load-limit parsing, recursion, scheduling, cancellation, and FIFO checks."""

import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


KATI = Path(os.environ.get('OMKTR_BINARY',
                         Path(__file__).resolve().parents[1] / 'omktr')).resolve()


class LoadLimits(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='omktr-load-')
        self.addCleanup(self.temp.cleanup)
        self.cwd = Path(self.temp.name)
        self.env = {k: v for k, v in os.environ.items()
                    if k not in ('MAKE', 'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'MAKEOVERRIDES')
                    and not k.startswith('KATI_')}
        self.env['TMPDIR'] = str(self.cwd)
        (self.cwd / 'worker.py').write_text(
            'import fcntl, json, time\n'
            'def update(delta):\n'
            '    with open("counts", "r+") as f:\n'
            '        fcntl.flock(f, fcntl.LOCK_EX)\n'
            '        active, peak, done = json.load(f)\n'
            '        active += delta\n'
            '        f.seek(0)\n'
            '        json.dump([active, max(peak, active), done + (delta < 0)], f)\n'
            '        f.truncate()\n'
            'update(1)\ntime.sleep(0.15)\nupdate(-1)\n')
        self.flat_makefile()

    def flat_makefile(self):
        (self.cwd / 'Makefile').write_text(
            '.PHONY: all a b c d\nall: a b c d\na b c d:\n\t@python3 worker.py\n')

    def run_tool(self, *args, env=None, success=True):
        result = subprocess.run([str(KATI), *args], cwd=self.cwd,
                                env={**self.env, **(env or {})}, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=20)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(list(self.cwd.glob('omktr-load-*')), [], result.stdout)
        return result.stdout

    def counts(self):
        return json.loads((self.cwd / 'counts').read_text())

    def reset_counts(self):
        (self.cwd / 'counts').write_text('[0, 0, 0]')

    def set_sample(self, value):
        temporary = self.cwd / 'sample.tmp'
        temporary.write_text(value)
        temporary.replace(self.sample)

    def controlled_load(self, sample='10'):
        source = self.cwd / 'load.c'
        library = self.cwd / 'load.so'
        self.sample = self.cwd / 'sample'
        self.set_sample(sample)
        source.write_text(
            '#include <stdio.h>\n#include <stdlib.h>\n'
            'int getloadavg(double *samples, int count) {\n'
            ' FILE *f = fopen(getenv("OMKTR_SAMPLE_FILE"), "r");\n'
            ' if (!f) return -1;\n'
            ' int ok = fscanf(f, "%lf", samples); fclose(f);\n'
            ' return ok == 1 ? 1 : -1;\n}\n')
        subprocess.run(['clang', '-shared', '-fPIC', '-fuse-ld=lld', str(source),
                        '-o', str(library)], check=True, capture_output=True)
        return {'LD_PRELOAD': str(library), 'OMKTR_SAMPLE_FILE': str(self.sample)}

    def wait_for(self, predicate, child):
        deadline = time.monotonic() + 5
        while not predicate():
            self.assertIsNone(child.poll())
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)

    def barrier_makefile(self):
        (self.cwd / 'Makefile').write_text(
            '.PHONY: all a b c d\nall: a b c d\na b c d:\n'
            '\t@python3 barrier.py $@\n')
        (self.cwd / 'barrier.py').write_text(
            'import sys,time\nfrom pathlib import Path\n'
            'name=sys.argv[1]\nPath("start-"+name).touch()\n'
            'while not Path("release-"+name).exists(): time.sleep(.01)\n'
            'Path("done-"+name).touch()\n')

    def stop_child(self, child):
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
        child.communicate()

    def test_forms_and_zero_serialization(self):
        for mode in ([], ['build']):
            for option in (['-l0'], ['-l', '0'], ['--load-average=0'],
                           ['--max-load=0'], ['-kl0']):
                with self.subTest(mode=mode, option=option):
                    self.reset_counts()
                    self.run_tool(*mode, '-j4', *option)
                    self.assertEqual(self.counts(), [0, 1, 4])

    def test_inheritance_and_overrides(self):
        for mode in ([], ['build']):
            for inherited in ('-l0', '-l 0', '--load-average=0', '--max-load=0', '-kl0'):
                with self.subTest(mode=mode, inherited=inherited):
                    self.reset_counts()
                    self.run_tool(*mode, '-j4', env={'MAKEFLAGS': inherited})
                    self.assertEqual(self.counts(), [0, 1, 4])
            for reset in ('-l', '--load-average', '--max-load', '-l1e100'):
                with self.subTest(mode=mode, reset=reset):
                    self.reset_counts()
                    self.run_tool(*mode, '-j4', reset, env={'MAKEFLAGS': '-l0'})
                    self.assertGreater(self.counts()[1], 1)
                    self.assertEqual(self.counts()[2], 4)
            self.reset_counts()
            self.run_tool(*mode, '-j4', '-l1e100', '-l0')
            self.assertEqual(self.counts(), [0, 1, 4])

    def test_invalid_limits(self):
        for value in ('-1', 'nan', 'inf', '1x', '', '1e999', '1e-999'):
            for mode in ([], ['build']):
                with self.subTest(value=value, mode=mode):
                    output = self.run_tool(*mode, '--load-average=' + value, success=False)
                    self.assertIn('Invalid load-average limit', output)
                    output = self.run_tool(*mode, env={'MAKEFLAGS': '--max-load=' + value},
                                           success=False)
                    self.assertIn('Invalid load-average limit', output)

    def test_failed_sampling_warns_once_and_serializes(self):
        env = self.controlled_load('failed')
        for mode in ([], ['build']):
            self.reset_counts()
            output = self.run_tool(*mode, '-j4', '-l1000000', env=env)
            self.assertEqual(self.counts(), [0, 1, 4])
            self.assertEqual(output.count('cannot read load average'), 1)

    def test_subnormal_limits(self):
        env = self.controlled_load('1')
        for mode in ([], ['build']):
            for value in ('1e-308', '5e-324'):
                self.reset_counts()
                self.run_tool(*mode, '-j4', '-l' + value, env=env)
                self.assertEqual(self.counts(), [0, 1, 4])

    def test_positive_load_transition_and_late_sampling_failure(self):
        env = {**self.env, **self.controlled_load()}
        self.barrier_makefile()
        for mode in ([], ['build']):
            for late_failure in (False, True):
                with self.subTest(mode=mode, late_failure=late_failure):
                    for pattern in ('start-*', 'done-*', 'release-*'):
                        for path in self.cwd.glob(pattern):
                            path.unlink()
                    self.set_sample('0' if late_failure else '10')
                    child = subprocess.Popen([str(KATI), *mode, '-j2', '-l2.5'],
                        cwd=self.cwd, env=env, start_new_session=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    try:
                        count = lambda: len(list(self.cwd.glob('start-*')))
                        self.wait_for(lambda: count() == (2 if late_failure else 1), child)
                        time.sleep(.25)
                        self.assertEqual(count(), 2 if late_failure else 1)
                        if not late_failure:
                            # A lower sample admits work before the first
                            # recipe completes, without a completion wakeup.
                            self.set_sample('0')
                            self.wait_for(lambda: count() == 2, child)
                        else:
                            self.set_sample('failed')
                            first = list(self.cwd.glob('start-*'))
                            (self.cwd / first[0].name.replace('start-', 'release-')).touch()
                            time.sleep(.3)
                            self.assertEqual(count(), 2)
                            (self.cwd / first[1].name.replace('start-', 'release-')).touch()
                            self.wait_for(lambda: count() == 3, child)
                            # Once sampling fails, later valid samples do not
                            # lift the conservative fallback for this executor.
                            self.set_sample('0')
                            time.sleep(.25)
                            self.assertEqual(count(), 3)
                        for name in 'abcd':
                            (self.cwd / ('release-' + name)).touch()
                        output, _ = child.communicate(timeout=5)
                        self.assertEqual(child.returncode, 0, output)
                        self.assertEqual(len(list(self.cwd.glob('done-*'))), 4)
                        self.assertEqual(output.count('cannot read load average'),
                                         1 if late_failure else 0)
                    finally:
                        self.stop_child(child)

    def test_recursive_override_and_fifo_return(self):
        (self.cwd / 'child.mk').write_text((self.cwd / 'Makefile').read_text())
        (self.cwd / 'Makefile').write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" -f child.mk $(CHILD_FLAGS)\n')
        fifo = self.cwd / 'fifo'
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        for mode in ([], ['build']):
            for child_flags, serial in (('', True), ('-l', False), ('-l1e100 -l0', True)):
                with self.subTest(mode=mode, child_flags=child_flags):
                    os.write(fd, b'xxxx')
                    self.reset_counts()
                    self.run_tool(*mode, '-j4', '-l0', 'CHILD_FLAGS=' + child_flags,
                                  env={'KATI_JOBSERVER_FIFO': str(fifo)})
                    peak = self.counts()[1]
                    self.assertEqual(self.counts()[2], 4)
                    if serial:
                        self.assertEqual(peak, 1)
                    else:
                        self.assertGreater(peak, 1)
                    self.assertEqual(len(os.read(fd, 32)), 4)

    def test_legacy_launcher_load_override(self):
        self.run_tool('--ninja', '--regen', '-j4', '-l1e100')
        self.reset_counts()
        result = subprocess.run(['sh', './ninja.sh', '-j4', '-l', '0'],
                                cwd=self.cwd, env=self.env, capture_output=True,
                                text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.counts(), [0, 1, 4])

    def test_launcher_end_of_options(self):
        for generation in ('', '-l1e100'):
            self.run_tool('--ninja', '--regen', '-j4', *([generation] if generation else []))
            for options in (['-j4'], ['-j4', '-l0']):
                for reserved in (False, True):
                    self.reset_counts()
                    result = subprocess.run(['sh', './ninja.sh', *options, '--', 'all'],
                        cwd=self.cwd, env={**self.env, **(
                            {'KATI_JOBSERVER_RESERVED': '1'} if reserved else {})},
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.counts()[2], 4)
                    if reserved or '-l0' in options:
                        self.assertEqual(self.counts()[1], 1)

    def test_nested_ninja_load_state_and_fifo_return(self):
        env = self.controlled_load('10')
        child = self.cwd / 'child'
        child.mkdir()
        (child / 'Makefile').write_text((self.cwd / 'Makefile').read_text())
        (child / 'worker.py').write_text((self.cwd / 'worker.py').read_text())
        (child / 'counts').symlink_to('../counts')
        (self.cwd / 'Makefile').write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" build -C child\n')
        fifo = self.cwd / 'fifo'
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        for mode in ([], ['build']):
            os.write(fd, b'xxxx')
            self.reset_counts()
            self.run_tool(*mode, '-j4', '-l2.5',
                          env={**env, 'KATI_JOBSERVER_FIFO': str(fifo)})
            self.assertEqual(self.counts(), [0, 1, 4])
            self.assertEqual(len(os.read(fd, 32)), 4)

    def test_launcher_preserves_ninja_operands_and_tool_arguments(self):
        self.run_tool('--ninja', '--regen', '-l1e100')
        (self.cwd / '-l0').write_text((self.cwd / 'build.ninja').read_text())
        for args in (['-f', '-l0', '-t', 'targets'], ['-t', 'commands'],
                     ['-tcommands'], ['-t', 'query', 'all']):
            result = subprocess.run(['sh', './ninja.sh', *args], cwd=self.cwd,
                                    env=self.env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_legacy_launcher_runtime_limit_reaches_recursive_children(self):
        (self.cwd / 'child.mk').write_text((self.cwd / 'Makefile').read_text())
        (self.cwd / 'Makefile').write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" -f child.mk\n')
        for generation, runtime, serial in (('1000000', '0', True),
                                             ('0', '1000000', False)):
            with self.subTest(generation=generation, runtime=runtime):
                self.run_tool('--ninja', '--regen', '-j4', '-l' + generation)
                self.reset_counts()
                result = subprocess.run(['sh', './ninja.sh', '-j4', '-l', runtime],
                                        cwd=self.cwd, env=self.env, capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.counts()[2], 4)
                if serial:
                    self.assertEqual(self.counts()[1], 1)
                else:
                    self.assertGreater(self.counts()[1], 1)
        # Clearing a runtime limit in a child must also clear it for that
        # child's children, rather than reviving the launcher's old value.
        (self.cwd / 'grandchild.mk').write_text((self.cwd / 'child.mk').read_text())
        (self.cwd / 'child.mk').write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" -f grandchild.mk\n')
        (self.cwd / 'Makefile').write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" -f child.mk -l\n')
        self.run_tool('--ninja', '--regen', '-j4', '-l1000000')
        self.reset_counts()
        result = subprocess.run(['sh', './ninja.sh', '-j4', '-l0'],
                                cwd=self.cwd, env=self.env, capture_output=True,
                                text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertGreater(self.counts()[1], 1)
        self.assertEqual(self.counts()[2], 4)

    def test_failure_and_signal_while_waiting(self):
        fifo = self.cwd / 'cancel-fifo'
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        env = {**self.env, **self.controlled_load('10'), 'KATI_JOBSERVER_FIFO': str(fifo)}
        for mode in ([], ['build']):
            os.write(fd, b'xxxx')
            (self.cwd / 'Makefile').write_text(
                '.PHONY: all a b c d\nall: a b c d\na b c d:\n'
                '\t@touch started; sleep 10\n')
            (self.cwd / 'started').unlink(missing_ok=True)
            child = subprocess.Popen([str(KATI), *mode, '-j4', '-l2.5'],
                                     cwd=self.cwd, env=env, start_new_session=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True)
            try:
                deadline = time.monotonic() + 10
                while not (self.cwd / 'started').exists():
                    self.assertIsNone(child.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.02)
                child.send_signal(signal.SIGTERM)
                output, _ = child.communicate(timeout=5)
                self.assertNotEqual(child.returncode, 0, output)
                self.assertEqual(list(self.cwd.glob('omktr-load-*')), [], output)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate()
            self.assertEqual(len(os.read(fd, 32)), 4)
            (self.cwd / 'Makefile').write_text(
                '.PHONY: all a b c d\nall: a b c d\na b c d:\n\t@false\n')
            os.write(fd, b'xxxx')
            self.run_tool(*mode, '-j4', '-l2.5', env=env, success=False)
            self.assertEqual(len(os.read(fd, 32)), 4)


if __name__ == '__main__':
    unittest.main()
