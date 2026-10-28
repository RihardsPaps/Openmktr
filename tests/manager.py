#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Portable manager checks; no Portage installation required."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('manager', Path(__file__).resolve().parents[1] / 'tools/omktr_manager.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class FakePortage:
    def atom(self, value):
        if '/' not in value or any(c in value for c in '\n\r '):
            raise ValueError('invalid atom')
        return value
    def packages(self):
        return [dict(package='cat/pkg', installed=True, available=True)]
    def matches(self, atom, package):
        return atom == package or atom == '*/*'

class ManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.backend = m.Backend(self.root, FakePortage())
    def apply(self, action='configure', packages=None, values=None):
        plan = self.backend.plan(action, packages or ['cat/pkg'], values or {})
        self.backend.apply(plan)
        return plan
    def test_snapshot_large_catalog_and_external_edit(self):
        from unittest import mock
        (self.root / 'package.env').write_text('cat/pkg custom\n')
        (self.root / 'env').mkdir()
        (self.root / 'env/custom').write_text('CFLAGS="-O2"\n')
        for count in (1000, 10000, 30000):
            with self.subTest(count=count), mock.patch.object(self.backend, 'read', wraps=self.backend.read) as read:
                snapshot = self.backend.snapshot()
                for index in range(count): snapshot.show(f'cat/pkg{index}')
                self.assertEqual(read.call_count, 2)
        snapshot = self.backend.snapshot()
        self.assertEqual(snapshot.show('cat/pkg')['state'], 'disabled')
        (self.root / 'env/custom').write_text('export MAKE=/usr/bin/omktr-portage\n')
        self.assertEqual(snapshot.show('cat/pkg')['state'], 'disabled')
        self.assertEqual(self.backend.snapshot().show('cat/pkg')['state'], 'indeterminate')

    def test_legacy_configuration_and_new_options(self):
        import hashlib
        values = m.settings({'jobs':'4'})
        name = m.PREFIX + hashlib.sha256(b'cat/pkg').hexdigest()[:24]
        (self.root / 'env').mkdir()
        path = self.root / 'env' / name
        path.write_text(m.environment(values, legacy=True))
        (self.root / 'package.env').write_text('cat/pkg ' + name + '\n')
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'managed')
        self.apply(values={'build_stats':True, 'regen_debug':True, 'disabled_graph_optimizations':'monitor,evaluation'})
        info = self.backend.show('cat/pkg')
        self.assertEqual(info['settings']['disabled_graph_optimizations'], 'evaluation,monitor')
        self.assertTrue(info['settings']['build_stats'])
        path.write_text(path.read_text() + '# custom edit\n')
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'indeterminate')
        with self.assertRaises(ValueError): m.settings({'disabled_graph_optimizations':'unknown'})

    def test_batch_and_idempotency(self):
        self.apply(packages=['cat/a', 'cat/b'])
        self.assertEqual(self.backend.show('cat/a')['state'], 'managed')
        self.assertFalse(self.backend.plan('configure', ['cat/a'], {})['changes'])
    def test_preserve_and_disable(self):
        path = self.root / 'package.env'
        path.write_text('# personal\nother/pkg custom\n')
        self.apply()
        self.apply('disable')
        self.assertIn('# personal\nother/pkg custom\n', path.read_text())
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'disabled')
    def test_directory(self):
        (self.root / 'package.env').mkdir()
        self.apply()
        self.assertTrue((self.root / 'package.env/openmktr-manager').exists())
    def test_stale_plan(self):
        plan = self.backend.plan('configure', ['cat/pkg'], {})
        (self.root / 'package.env').write_text('# concurrent edit\n')
        with self.assertRaises(RuntimeError): self.backend.apply(plan)
    def test_injection(self):
        for values in ({'load':'1; touch /tmp/bad'}, {'root':'/var/cache/openmktr/../../etc'}, {'jobs':'0'}, {'strict':'true'}, {'workspace':True}):
            with self.subTest(values=values), self.assertRaises(ValueError): m.settings(values)
    def test_path_escape(self):
        with self.assertRaises(RuntimeError): self.backend.read('../outside')
    def test_conflict(self):
        (self.root / 'package.env').write_text('*/* custom\n')
        with self.assertRaises(RuntimeError): self.apply()
    def test_adoption(self):
        (self.root / 'env').mkdir()
        (self.root / 'env/openmktr-direct').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="direct"\n')
        (self.root / 'env/custom').write_text('CFLAGS="-O2"\n')
        (self.root / 'package.env').write_text('*/* custom\ncat/pkg custom openmktr-direct\n')
        self.apply('adopt')
        self.assertIn('cat/pkg custom\n', (self.root / 'package.env').read_text())
        self.assertTrue((self.root / 'env/openmktr-direct').exists())
        self.assertIn('*/* custom\n', (self.root / 'package.env').read_text())
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'managed')
    def test_interrupted_recovery_preserves_manual_changes(self):
        import json
        plan = self.backend.plan('configure', ['cat/pkg'], {})
        journal = self.root / '.openmktr-manager-transaction'
        journal.write_text(json.dumps(plan))
        (self.root / 'package.env').write_text('# intervening manual change\n')
        with self.assertRaises(RuntimeError): self.backend.recover()
        self.assertTrue(journal.exists())
        self.assertEqual((self.root / 'package.env').read_text(), '# intervening manual change\n')
    def test_recovery_of_partial_publication(self):
        import json
        plan = self.backend.plan('configure', ['cat/pkg'], {})
        journal = self.root / '.openmktr-manager-transaction'
        journal.write_text(json.dumps(plan))
        for relative, change in plan['changes'].items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(change['after'])
        self.backend.recover()
        self.assertFalse(journal.exists())
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'disabled')
    def test_modified_environment_is_not_overwritten(self):
        self.apply()
        path = next((self.root / 'env').iterdir())
        path.write_text(path.read_text() + 'export MAKE=/custom\n')
        with self.assertRaises(RuntimeError): self.apply()
    def test_helper_rejects_forged_changes(self):
        plan = self.backend.plan('configure', ['cat/pkg'], {})
        plan['changes']['../outside'] = dict(before='', after='bad')
        with self.assertRaises(RuntimeError): self.backend.apply(plan)
    def test_workspace_request_validation(self):
        for request in ({'action':'run'}, {'root':'/etc'}, {'command':'bad'}):
            with self.assertRaises(ValueError): m.workspace(request, elevate=False)
    def test_curses_search_select_review_and_failed_save(self):
        import types
        from unittest import mock
        keys = iter([ord('/'), ord(' '), ord('c'), ord('r'), 10, ord('s'), ord('q')])
        replies = iter([b'cat', *([b''] * len(m.OPTIONS)), b'yes'])
        screen = mock.Mock()
        screen.getmaxyx.return_value = (25, 100)
        screen.getch.side_effect = lambda: next(keys)
        screen.getstr.side_effect = lambda *args: next(replies)
        fake = types.SimpleNamespace(KEY_DOWN=258, KEY_UP=259, echo=lambda: None,
                                     noecho=lambda: None, wrapper=lambda run: run(screen))
        with mock.patch.dict('sys.modules', curses=fake), mock.patch.object(m, 'save', side_effect=RuntimeError('cancelled')) as save:
            m.tui(self.backend)
            save.assert_called_once()
            self.assertIsNotNone(save.call_args.args[1])
    def test_sparse_draft_reset_and_mixed_boolean(self):
        draft = m.SettingsDraft(dict(jobs=None, strict=None, mode='ninja'))
        draft.set('jobs', '4')
        draft.set('strict', False)
        draft.set('mode', 'ninja')
        self.assertEqual(draft.edits, dict(jobs='4', strict=False))
        draft.set('jobs', None)
        self.assertEqual(draft.edits, dict(strict=False))

    def test_harmless_inherited_environment_remains_manageable(self):
        (self.root / 'env').mkdir()
        (self.root / 'env/custom').write_text('CFLAGS="-O2"\n')
        (self.root / 'package.env').write_text('*/* custom\n')
        self.apply()
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'managed')
        self.apply(values={'jobs': '4'})
        self.apply('disable')
        self.assertEqual(self.backend.show('cat/pkg')['state'], 'disabled')
        self.assertIn('*/* custom\n', (self.root / 'package.env').read_text())

    def test_overlapping_activation_is_still_rejected(self):
        (self.root / 'env').mkdir()
        (self.root / 'env/openmktr-direct').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="direct"\n')
        (self.root / 'package.env').write_text('*/* openmktr-direct\n')
        with self.assertRaises(RuntimeError): self.apply('adopt')
        self.assertNotIn(m.BEGIN, (self.root / 'package.env').read_text())

    def test_edit_initial_values_and_sparse_batch_overrides(self):
        self.apply(packages=['cat/a'], values={'mode':'ninja', 'strict':True, 'jobs':'8'})
        self.apply(packages=['cat/b'], values={'mode':'direct', 'jobs':'2'})
        controller = m.Controller(self.backend)
        initial = controller.initial_settings(['cat/a', 'cat/b'])
        self.assertIsNone(initial['mode'])
        self.assertIsNone(initial['strict'])
        self.assertEqual(controller.initial_settings(['cat/a'])['mode'], 'ninja')
        controller.stage('configure', ['cat/a','cat/b'], {'jobs':'4'})
        self.backend.apply(controller.pending)
        self.assertEqual(self.backend.show('cat/a')['settings']['mode'], 'ninja')
        self.assertTrue(self.backend.show('cat/a')['settings']['strict'])
        self.assertEqual(self.backend.show('cat/b')['settings']['mode'], 'direct')

    def test_adoption_initial_settings_preserve_manual_ninja(self):
        (self.root / 'env').mkdir()
        (self.root / 'env/openmktr-ninja').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="ninja"\n')
        (self.root / 'package.env').write_text('cat/pkg openmktr-ninja\n')
        controller = m.Controller(self.backend)
        self.assertEqual(controller.initial_settings(['cat/pkg'])['mode'], 'ninja')
        controller.stage('adopt', ['cat/pkg'], {})
        self.backend.apply(controller.pending)
        self.assertEqual(self.backend.show('cat/pkg')['settings']['mode'], 'ninja')

    def test_failed_save_retains_pending(self):
        c = m.Controller(self.backend)
        c.stage('configure', ['cat/pkg'], {})
        old = m.save
        def fail(*args): raise RuntimeError('authentication cancelled')
        m.save = fail
        try:
            with self.assertRaises(RuntimeError): c.save()
            self.assertIsNotNone(c.pending)
        finally: m.save = old

if __name__ == '__main__': unittest.main()
