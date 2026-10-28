#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Optional real Tk widget smoke check; run with a display (or under Xvfb)."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import tkinter as tk
from tkinter import simpledialog, messagebox

spec = importlib.util.spec_from_file_location('manager_checks', Path(__file__).with_name('manager.py'))
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)
m = checks.m

class NativeTk(unittest.TestCase):
    def test_search_batch_review_authentication_failure_and_discard(self):
        root = tk.Tk()
        root.withdraw()
        with tempfile.TemporaryDirectory() as directory:
            backend = m.Backend(Path(directory), checks.FakePortage())
            def interact():
                root.update_idletasks()
                widgets = root.winfo_children()
                listing = next(w for w in widgets if isinstance(w, tk.Listbox))
                preview = next(w for w in widgets if isinstance(w, tk.Text))
                buttons = {w.cget('text'): w for w in widgets if w.winfo_class() == 'TButton'}
                self.assertIn('cat/pkg', listing.get(0))
                listing.selection_set(0)
                buttons['Configure'].invoke()
                self.assertIn('BEGIN openmktr-manager', preview.get('1.0', 'end'))
                buttons['Save'].invoke()
                errors.assert_called_once()
                self.assertIn('BEGIN openmktr-manager', preview.get('1.0', 'end'))
                buttons['Discard edits'].invoke()
                self.assertEqual(preview.get('1.0', 'end').strip(), '')
                root.destroy()
            try:
                with mock.patch.object(tk, 'Tk', return_value=root), mock.patch.object(root, 'mainloop', side_effect=interact), mock.patch.object(simpledialog, 'askstring', side_effect=lambda *a, **kw: kw.get('initialvalue', '')), mock.patch.object(messagebox, 'showerror') as errors, mock.patch.object(messagebox, 'askyesno', return_value=True), mock.patch.object(m, 'save', side_effect=RuntimeError('authentication cancelled')):
                    m.gui(backend)
            finally:
                try: root.destroy()
                except tk.TclError: pass

    def test_managed_batch_and_manual_ninja_dialogs_preserve_other_settings(self):
        for scenario in ('managed', 'batch', 'adopt'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as directory:
                root = tk.Tk()
                root.withdraw()
                catalog = checks.FakePortage()
                packages = ['cat/a', 'cat/b'] if scenario == 'batch' else ['cat/pkg']
                catalog.packages = lambda: [dict(package=p, installed=True, available=True) for p in packages]
                backend = m.Backend(Path(directory), catalog)
                def publish(backend, plan):
                    for relative, change in plan['changes'].items():
                        path = backend.root / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(change['after'])
                (backend.root / 'bashrc').write_text(m.BEGIN)
                if scenario == 'adopt':
                    (backend.root / 'env').mkdir()
                    (backend.root / 'env/openmktr-ninja').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="ninja"\n')
                    (backend.root / 'package.env').write_text('cat/pkg openmktr-ninja\n')
                else:
                    for index, package in enumerate(packages):
                        values = dict(mode='ninja' if index == 0 else 'direct', strict=index == 0,
                                      workspace=index == 0, jobs='8' if index == 0 else '2')
                        publish(backend, backend.plan('configure', [package], values))
                initial = {}
                def answer(name, *args, **kwargs):
                    initial[name] = kwargs['initialvalue']
                    return '4' if name == 'jobs' else kwargs['initialvalue']
                def interact():
                    root.update_idletasks()
                    widgets = root.winfo_children()
                    listing = next(w for w in widgets if isinstance(w, tk.Listbox))
                    buttons = {w.cget('text'): w for w in widgets if w.winfo_class() == 'TButton'}
                    listing.selection_set(0, 'end')
                    buttons['Adopt' if scenario == 'adopt' else 'Configure'].invoke()
                    buttons['Save'].invoke()
                    errors.assert_not_called()
                    plan = save.call_args.args[1]
                    self.assertEqual(plan['values'], {'jobs':'4'})
                    for index, package in enumerate(packages):
                        values = backend.show(package)['settings']
                        self.assertEqual(values['mode'], 'ninja' if index == 0 else 'direct')
                        self.assertEqual(values['jobs'], '4')
                        if scenario != 'adopt':
                            self.assertEqual(values['strict'], index == 0)
                            self.assertEqual(values['workspace'], index == 0)
                    self.assertEqual(initial['mode'], '<mixed; unchanged>' if scenario == 'batch' else 'ninja')
                    root.destroy()
                try:
                    with mock.patch.object(tk, 'Tk', return_value=root), mock.patch.object(root, 'mainloop', side_effect=interact), mock.patch.object(simpledialog, 'askstring', side_effect=answer), mock.patch.object(messagebox, 'showerror') as errors, mock.patch.object(m, 'save', side_effect=publish) as save:
                        m.gui(backend)
                finally:
                    try: root.destroy()
                    except tk.TclError: pass

if __name__ == '__main__': unittest.main()
