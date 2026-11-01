#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Portable checks for the Portage adapter; no Portage installation required."""

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
KATI = Path(os.environ.get('OMKTR_BINARY', ROOT / 'omktr')).resolve()
WRAPPER = ROOT / 'packaging/gentoo/omktr-portage'


class PortageWrapper(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='omktr portage ')
        self.addCleanup(self.temp.cleanup)
        self.cwd = Path(self.temp.name)
        self.bindir = self.cwd / 'bin with spaces'
        self.bindir.mkdir()
        self.wrapper = self.bindir / 'omktr-portage'
        shutil.copyfile(WRAPPER, self.wrapper)
        self.wrapper.chmod(0o755)
        self.binary = self.bindir / 'omktr'
        self.binary.write_text(
            '#!/usr/bin/env python3\n'
            'import json, os, sys, time\n'
            'from pathlib import Path\n'
            'if os.environ.get("WAIT"):\n'
            '    Path("ready").touch()\n'
            '    time.sleep(60)\n'
            'print(json.dumps({"args": sys.argv[1:], "cwd": os.getcwd(),\n'
            '                  "env": {k: os.environ.get(k) for k in\n'
            '                  ("MAKE", "MAKEOPTS", "EXTRA_EMAKE", "CC", "CXX", "CFLAGS")}}))\n'
            'sys.exit(int(os.environ.get("STATUS", "0")))\n', encoding='utf-8')
        self.binary.chmod(0o755)
        self.env = {k: v for k, v in os.environ.items()
                    if k not in ('MAKE', 'MAKEFLAGS', 'MAKEOVERRIDES',
                                 'EBUILD_PHASE', 'OMKTR_PORTAGE_MODE')
                    and not k.startswith('KATI_')}

    def invoke(self, *args, **env):
        return subprocess.run(['sh', str(self.wrapper), *args], cwd=self.cwd,
                              env={**self.env, **env}, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=15)

    def test_manager_diagnostics_and_exclusions(self):
        for mode in ('direct', 'ninja'):
            result = self.invoke('all', OMKTR_PORTAGE_MODE=mode, EBUILD_PHASE='compile', OMKTR_PORTAGE_BUILD_STATS='1', OMKTR_PORTAGE_REGEN_DEBUG='1', OMKTR_PORTAGE_DISABLED_GRAPH_OPTIMIZATIONS='evaluation,monitor')
            self.assertEqual(result.returncode, 0, result.stderr)
            arguments = json.loads(result.stdout)['args']
            for flag in ('--kati_stats', '--regen_debug', '--disable_graph_optimization=evaluation', '--disable_graph_optimization=monitor'): self.assertIn(flag, arguments)
        self.assertEqual(self.invoke(OMKTR_PORTAGE_DISABLED_GRAPH_OPTIMIZATIONS='bad').returncode, 2)

    def test_modes_and_phases(self):
        for mode in ('direct', 'ninja'):
            for phase in ('', 'setup', 'configure', 'compile', 'test', 'install'):
                with self.subTest(mode=mode, phase=phase):
                    result = self.invoke('all', OMKTR_PORTAGE_MODE=mode,
                                         EBUILD_PHASE=phase)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    prefix = ['build'] if mode == 'ninja' and phase == 'compile' else []
                    self.assertEqual(json.loads(result.stdout)['args'], prefix + ['all'])
        self.assertEqual(json.loads(self.invoke('all').stdout)['args'], ['all'])

    def test_manager_defaults_precede_explicit_arguments(self):
        result = self.invoke('--graph_optimizations=off', '-j1',
                             OMKTR_PORTAGE_MODE='ninja', EBUILD_PHASE='compile',
                             OMKTR_PORTAGE_GRAPH_OPTIMIZATIONS='safe',
                             OMKTR_PORTAGE_RECURSIVE_CACHE='1',
                             OMKTR_PORTAGE_STRICT_COMPAT='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['args'],
                         ['build', '--strict-compat', '--recursive_cache',
                          '--graph_optimizations=safe', '--graph_optimizations=off', '-j1'])
        self.assertEqual(self.invoke(OMKTR_PORTAGE_RECURSIVE_CACHE='bad').returncode, 2)

    def test_arguments_environment_and_cwd(self):
        args = ['-j4', '-l2.5', '-C', 'a b', 'DESTDIR=/image with spaces',
                'VALUE=$literal; *', 'MAKE=/explicit/tool', 'install']
        result = self.invoke(*args, MAKEOPTS='-j7', EXTRA_EMAKE='EXTRA=once',
                             CC='clang', CXX='clang++', CFLAGS='-O2 -pipe')
        record = json.loads(result.stdout)
        self.assertEqual(record['args'], args)
        self.assertEqual(record['cwd'], str(self.cwd))
        for name, value in dict(MAKE=str(self.binary), MAKEOPTS='-j7',
                                EXTRA_EMAKE='EXTRA=once', CC='clang',
                                CXX='clang++', CFLAGS='-O2 -pipe').items():
            self.assertEqual(record['env'][name], value)

    def test_path_lookup(self):
        result = subprocess.run(['omktr-portage', 'all'], cwd=self.cwd,
                                env={**self.env, 'PATH': str(self.bindir) +
                                     os.pathsep + self.env['PATH']},
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['env']['MAKE'], str(self.binary))

    def test_physical_directory_lookup(self):
        alias = self.cwd / 'directory alias'
        alias.symlink_to(self.bindir, target_is_directory=True)
        result = subprocess.run(['sh', str(alias / 'omktr-portage'), 'all'],
                                cwd=self.cwd, env=self.env, text=True,
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['env']['MAKE'], str(self.binary))

    @unittest.skipUnless(os.environ.get('OMKTR_EXPECT_NINJA_EXECUTABLE'),
                         'requires a binary configured with an absolute Ninja path')
    def test_configured_ninja_ignores_path_alternative(self):
        expected = os.environ['OMKTR_EXPECT_NINJA_EXECUTABLE']
        self.assertTrue(Path(expected).is_absolute())
        shutil.copyfile(KATI, self.binary)
        alternative = self.cwd / 'alternative'
        alternative.mkdir()
        (alternative / 'ninja').write_text(
            '#!/bin/sh\nprintf "%s\\n" "wrong Ninja alternative" >&2\nexit 99\n')
        (alternative / 'ninja').chmod(0o755)
        (self.cwd / 'Makefile').write_text('all:\n\t@printf done > result\n')
        result = self.invoke('-j2', '-l0.001', OMKTR_PORTAGE_MODE='ninja',
                             EBUILD_PHASE='compile',
                             PATH=str(alternative) + os.pathsep + self.env['PATH'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.cwd / 'result').read_text(), 'done')
        bundle = (self.cwd / '.openmktr/build.ninja').resolve().parent
        self.assertIn(expected.encode(), (bundle / 'ninja.sh.launch').read_bytes())

    def test_invalid_mode_missing_binary_and_failure(self):
        result = self.invoke(OMKTR_PORTAGE_MODE='unknown')
        self.assertEqual(result.returncode, 2)
        self.assertIn('direct or ninja', result.stderr)
        self.assertEqual(self.invoke('all', STATUS='23').returncode, 23)
        self.binary.unlink()
        self.assertEqual(self.invoke('all').returncode, 127)

    def test_signal_reaches_executed_process(self):
        child = subprocess.Popen(['sh', str(self.wrapper)], cwd=self.cwd,
                                 env={**self.env, 'WAIT': '1'})
        try:
            deadline = time.monotonic() + 10
            while not (self.cwd / 'ready').exists():
                self.assertIsNone(child.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            child.send_signal(signal.SIGTERM)
            self.assertEqual(child.wait(timeout=5), -signal.SIGTERM)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()

    def test_real_recursion_and_staged_install(self):
        # The fake executable tests quoting separately. Recursive MAKE path
        # handling itself retains Openmktr's existing path constraints.
        shutil.copyfile(KATI, self.binary)
        for mode in ('direct', 'ninja'):
            with self.subTest(mode=mode):
                (self.cwd / 'Makefile').write_text(
                    '.PHONY: all\nall:\n\t@"$(MAKE)" -f child.mk install\n')
                (self.cwd / 'child.mk').write_text(
                    '.PHONY: install\ninstall:\n'
                    '\t@mkdir -p "$(DESTDIR)"; printf "%s" "$(CC)" > "$(DESTDIR)/tool"\n')
                image = self.cwd / ('image ' + mode)
                result = self.invoke('-j2', '-l0', 'DESTDIR=' + str(image),
                                     CC='clang', OMKTR_PORTAGE_MODE=mode,
                                     EBUILD_PHASE='compile')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((image / 'tool').read_text(), 'clang')
                self.assertFalse((self.cwd / 'build.ninja').exists())
        self.assertTrue((self.cwd / '.openmktr/build.ninja').exists())
        result = self.invoke('-q', OMKTR_PORTAGE_MODE='ninja', EBUILD_PHASE='compile')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('does not support', result.stderr)

    def test_lifecycle_fixture_before_portage_merge(self):
        shutil.copyfile(KATI, self.binary)
        files = ROOT / 'validation/compatibility/portage-fixture/files'
        for mode in ('direct', 'ninja'):
            with self.subTest(mode=mode):
                work = self.cwd / mode
                shutil.copytree(files, work)
                env = {**self.env, 'MAKE': str(self.wrapper),
                       'OMKTR_PORTAGE_MODE': mode, 'CC': 'clang',
                       'CFLAGS': '-O1 -DPORTAGE_FIXTURE=7'}

                def run(command, phase, success=True):
                    result = subprocess.run(command, cwd=work,
                                            env={**env, 'EBUILD_PHASE': phase},
                                            text=True, capture_output=True, timeout=20)
                    if success:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                    return result

                def compile(*args):
                    run(['python3', 'worker.py', 'reset'], 'compile')
                    run(['sh', str(self.wrapper), '-j4', '-l1000000', *args], 'compile')

                run(['sh', './configure'], 'configure')
                compile()
                run(['python3', 'worker.py', 'verify', 'parallel'], 'compile')
                before = (work / 'fixture').stat().st_mtime_ns
                compile('-j1')
                run(['python3', 'worker.py', 'verify', 'serial'], 'compile')
                self.assertEqual((work / 'fixture').stat().st_mtime_ns, before)
                (work / 'config.mk').write_text('FIXTURE_VERSION = 2\n')
                compile('-l0')
                run(['python3', 'worker.py', 'verify', 'serial'], 'compile')
                self.assertEqual(run(['./fixture'], 'compile').stdout.strip(),
                                 'marker=7 version=2')
                run(['sh', str(self.wrapper), '-n', 'check'], 'test')
                self.assertFalse((work / 'tested').exists())
                run(['sh', str(self.wrapper), 'check'], 'test')
                self.assertTrue((work / 'tested').exists())
                image = work / 'image with spaces'
                run(['sh', str(self.wrapper), 'DESTDIR=' + str(image),
                     'PACKAGE=example', 'install'], 'install')
                self.assertTrue((image / 'usr/bin/example').is_file())
                for phase in ('configure', 'compile', 'test', 'install'):
                    self.assertEqual((work / (phase + '-phase')).read_text(), phase)
                self.assertEqual((work / 'build.ninja').read_text(), 'unrelated graph\n')
                self.assertEqual((work / '.openmktr').exists(), mode == 'ninja')
                failure = run(['sh', str(self.wrapper), 'fail'], 'compile', success=False)
                self.assertIn('intentional Portage fixture failure',
                              failure.stdout + failure.stderr)


class PortageBuildFlags(unittest.TestCase):
    def test_build_graph_preserves_flags_without_modifying_base(self):
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            shutil.copyfile(ROOT / 'build.ninja', cwd / 'build.ninja')
            original = (cwd / 'build.ninja').read_bytes()
            subprocess.run([os.sys.executable, str(ROOT / 'packaging/gentoo/configure_build.py')],
                           cwd=cwd, env={**os.environ, 'CPPFLAGS': '-DUSER_FLAG=1',
                                         'CXXFLAGS': '-O1',
                                         'LDFLAGS': "-Wl,-rpath,'$ORIGIN'"}, check=True)
            commands = subprocess.check_output(
                ['ninja', '-f', 'build.portage.ninja', '-t', 'commands', 'omktr'],
                cwd=cwd, text=True)
            self.assertIn('-DUSER_FLAG=1', commands)
            self.assertIn('-O1', commands)
            self.assertIn('-stdlib=libc++', commands)
            self.assertIn('-DOMKTR_NINJA_EXECUTABLE="/usr/bin/ninja-reference"', commands)
            self.assertIn('$ORIGIN', commands)
            self.assertNotIn('$$ORIGIN', commands)
            self.assertEqual((cwd / 'build.ninja').read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
