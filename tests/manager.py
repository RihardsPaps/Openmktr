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
    def test_tk_review_and_failed_save(self):
        import types
        from unittest import mock
        buttons = {}
        root = mock.Mock()
        listing = mock.Mock()
        listing.curselection.return_value = (0,)
        preview = mock.Mock()
        def button(parent, text, command):
            buttons[text] = command
            return mock.Mock()
        def variable(value=''):
            result = mock.Mock()
            result.get.return_value = value
            return result
        dialogs = types.SimpleNamespace(askstring=lambda *args, **kw: kw.get('initialvalue', ''))
        messages = types.SimpleNamespace(showerror=mock.Mock(), askyesno=lambda *a: True)
        ttk = types.SimpleNamespace(Entry=mock.Mock(), Combobox=mock.Mock(), Button=button)
        tk = types.SimpleNamespace(Tk=lambda: root, StringVar=variable, Listbox=lambda *a, **kw: listing,
                                   Text=lambda *a, **kw: preview, ttk=ttk,
                                   messagebox=messages, simpledialog=dialogs)
        def interact():
            buttons['Configure']()
            self.assertIn('BEGIN openmktr-manager', preview.insert.call_args.args[1])
            buttons['Save']()
            messages.showerror.assert_called_once()
            buttons['Cancel']()
        root.mainloop.side_effect = interact
        with mock.patch.dict('sys.modules', tkinter=tk), mock.patch.object(m, 'save', side_effect=RuntimeError('cancelled')):
            m.gui(self.backend)
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
