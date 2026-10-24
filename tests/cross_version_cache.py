#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Portable failure/accounting checks; no compiler cache or Portage required."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


HERE = Path(__file__).resolve().parents[1] / 'validation/compatibility'
SPEC = importlib.util.spec_from_file_location('reuse_campaign', HERE / 'cross_version_cache.py')
CACHE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CACHE)


class CacheEvidenceTests(unittest.TestCase):
    def test_selected_binary_does_not_claim_ebuild_installation_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'evidence'
            with CACHE.PORTAGE.evidence_run(output, openmktr_use_test=False):
                pass
            result = json.loads((output / 'result.json').read_text())
            self.assertFalse(result['openmktr_use_test'])
            self.assertEqual(result['status'], 'passed')

    def test_changed_or_missing_output_is_a_failure(self):
        for actual in ({'unit.o': 'stale'}, {}, {'unit.o': 'right', 'extra.o': 'unexpected'}):
            with self.assertRaisesRegex(RuntimeError, 'comparison mismatch'):
                CACHE.require_equal({'unit.o': 'right'}, actual, 'regression')

    def test_repetition_never_inherits_the_new_version_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed, target = root / 'seed', root / 'cache'
            seed.mkdir()
            (seed / 'old').write_text('old-version result')
            CACHE.restore_seed(seed, target)
            (target / 'new').write_text('new-version result')
            (target / 'old').write_text('mutated')
            CACHE.restore_seed(seed, target)
            self.assertFalse((target / 'new').exists())
            self.assertEqual((target / 'old').read_text(), 'old-version result')
            self.assertEqual((seed / 'old').read_text(), 'old-version result')
            with self.assertRaises(ValueError):
                CACHE.restore_seed(seed, seed)

    def test_cache_counters_exclude_seed_and_timestamp_totals(self):
        before = CACHE.counters('preprocessed_cache_hit 30\ncache_miss 10\nstats_updated_timestamp 123\n')
        after = CACHE.counters('preprocessed_cache_hit 34\ncache_miss 11\nstats_updated_timestamp 127\n')
        self.assertEqual(CACHE.counter_delta(before, after),
                         {'preprocessed_cache_hit': 4, 'cache_miss': 1})

    def test_summary_uses_each_complete_sample_total(self):
        rows = [{'package': 'gzip', 'mode': 'direct', 'profile': 'original',
                 'cache_state': 'upgrade', 'build_seconds': total,
                 'compile_seconds': compile_time, 'cpu_seconds': total * 2,
                 'cache_stats': {'preprocessed_cache_hit': 2}}
                for total, compile_time in ((10, 8), (12, 5), (11, 9))]
        summary = CACHE.summarize(rows)['gzip/direct/original/upgrade']
        self.assertEqual(summary['samples'], 3)
        self.assertEqual(summary['build_seconds'], {'median': 11, 'min': 10, 'max': 12})
        self.assertEqual(summary['compile_seconds']['median'], 8)
        self.assertEqual(summary['cache_hits'], [2, 2, 2])

    def test_failure_is_persisted_in_existing_evidence_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'evidence'
            with self.assertRaisesRegex(RuntimeError, 'comparison mismatch'):
                with CACHE.PORTAGE.evidence_run(output):
                    CACHE.require_equal({'a.o': 'clean'}, {'a.o': 'stale'}, 'test')
            result = json.loads((output / 'result.json').read_text())
            self.assertEqual(result['status'], 'failed')
            self.assertIn('comparison mismatch', result['error'])


if __name__ == '__main__':
    unittest.main()
