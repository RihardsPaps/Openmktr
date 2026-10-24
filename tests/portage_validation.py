#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Portable checks for lifecycle evidence failure and retry handling."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


MODULE = Path(__file__).resolve().parents[1] / 'validation/compatibility/portage.py'
SPEC = importlib.util.spec_from_file_location('portage_validation', MODULE)
PORTAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PORTAGE)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='omktr-evidence-')
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / 'evidence'

    def result(self):
        return json.loads((self.output / 'result.json').read_text())

    def records(self):
        return [json.loads(line) for line in
                (self.output / 'commands.jsonl').read_text().splitlines()]

    def test_refuse_previous_evidence_without_modifying_it(self):
        with PORTAGE.evidence_run(self.output):
            pass
        previous = (self.output / 'result.json').read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'must be empty'):
            with PORTAGE.evidence_run(self.output):
                self.fail('reused evidence directory was accepted')
        self.assertEqual((self.output / 'result.json').read_bytes(), previous)

    def test_failure_replaces_running_status(self):
        with self.assertRaisesRegex(RuntimeError, 'unexpected exit 7'):
            with PORTAGE.evidence_run(self.output):
                self.assertEqual(self.result()['status'], 'running')
                PORTAGE.run([sys.executable, '-c', 'raise SystemExit(7)'],
                            self.output, 'failure')
        self.assertEqual(self.result()['status'], 'failed')
        self.assertEqual(self.records()[0]['exit_status'], 7)
        self.assertFalse((self.output / 'result.json.tmp').exists())

    def test_expected_failure_is_recorded_and_can_pass(self):
        with PORTAGE.evidence_run(self.output):
            PORTAGE.run([sys.executable, '-c', 'raise SystemExit(3)'],
                        self.output, 'expected-failure', success=False)
        self.assertEqual(self.result()['status'], 'passed')
        self.assertFalse(self.records()[0]['expected_success'])

    def test_timeout_records_outcome_and_kills_descendants(self):
        marker = Path(self.temporary.name) / 'late-write'
        child = ('import time; from pathlib import Path; time.sleep(.4); '
                 f'Path({str(marker)!r}).write_text("leaked")')
        parent = ('import subprocess,sys,time; '
                  f'subprocess.Popen([sys.executable,"-c",{child!r}]); time.sleep(30)')
        with self.assertRaises(subprocess.TimeoutExpired):
            with PORTAGE.evidence_run(self.output):
                PORTAGE.run([sys.executable, '-c', parent], self.output,
                            'timeout', timeout=.1)
        time.sleep(.5)
        self.assertFalse(marker.exists())
        self.assertTrue(self.records()[0]['timed_out'])
        self.assertNotEqual(self.records()[0]['exit_status'], 0)
        self.assertEqual(self.result()['status'], 'failed')

    def test_sandbox_downgrade_fails_even_with_zero_exit(self):
        with self.assertRaisesRegex(RuntimeError, 'namespace sandboxes'):
            with PORTAGE.evidence_run(self.output):
                PORTAGE.run([sys.executable, '-c',
                             'print("Unable to configure loopback interface: EPERM")'],
                            self.output, 'sandbox')
        self.assertEqual(self.records()[0]['exit_status'], 0)
        self.assertEqual(self.result()['status'], 'failed')

    def test_real_packages_require_fresh_compile_and_tests(self):
        log = ('>>> Source configured.\n>>> Compiling source\n'
               '/usr/bin/omktr-portage -j4\n>>> Source compiled.\n'
               '>>> Test phase: package\n>>> Completed testing package\n'
               '>>> Completed installing package\n')
        PORTAGE.verify_real_phases(log, 'package')
        with self.assertRaisesRegex(RuntimeError, 'phase was skipped'):
            PORTAGE.verify_real_phases(log.replace('>>> Test phase:', 'Already tested:'),
                                       'package')
        with self.assertRaisesRegex(RuntimeError, 'compile did not exercise'):
            PORTAGE.verify_real_phases(log.replace('/usr/bin/omktr-portage -j4', 'make -j4')
                                       + '/usr/bin/omktr-portage install\n', 'package')

    def test_namespace_diagnostics_are_not_matched_inside_echoed_commands(self):
        diagnostic = 'Unable to configure loopback interface: EPERM'
        self.assertFalse(PORTAGE.namespace_warning('+ python -c print(' + repr(diagnostic) + ')'))
        self.assertFalse(PORTAGE.namespace_warning('example: ' + diagnostic))
        self.assertTrue(PORTAGE.namespace_warning('\x1b[31m * ' + diagnostic + '\x1b[0m'))
        self.assertTrue(PORTAGE.namespace_warning('Calculating dependencies ...' + diagnostic))
        with PORTAGE.evidence_run(self.output):
            PORTAGE.run([sys.executable, '-c', 'print("example: " + ' + repr(diagnostic) + ')'],
                        self.output, 'quoted-diagnostic')
        self.assertEqual(self.result()['status'], 'passed')


if __name__ == '__main__':
    unittest.main()
