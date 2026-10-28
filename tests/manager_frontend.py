#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Real widget checks with a private display and disposable configuration."""
import importlib.util
from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock
import tkinter as tk

spec = importlib.util.spec_from_file_location('checks', Path(__file__).with_name('manager.py'))
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)
m = checks.m

class NativeTk(unittest.TestCase):
    def test_editor_review_failure_batch_and_workspace(self):
        root = tk.Tk()
        failure = []
        root.report_callback_exception = lambda *args: failure.append(args[1])
        with tempfile.TemporaryDirectory() as directory:
            catalog = checks.FakePortage()
            catalog.packages = lambda: [dict(package=p, installed=True, available=True) for p in ('cat/a', 'cat/b')]
            backend = m.Backend(Path(directory), catalog)
            for p, mode in (('cat/a', 'ninja'), ('cat/b', 'direct')):
                backend.apply(backend.plan('configure', [p], dict(mode=mode, jobs='8')))
            def wait(predicate):
                deadline = time.monotonic() + 8
                while not predicate():
                    if time.monotonic() > deadline: self.fail('UI operation timed out')
                    root.update()
                    time.sleep(.02)
            def interact():
                try:
                    view = root.manager
                    wait(lambda: len(view['table'].get_children()) == 2)
                    view['table'].selection_set(('cat/a', 'cat/b')); root.update()
                    self.assertEqual(view['variables']['mode'].get(), 'Mixed (unchanged)')
                    performance = view['pages']['Performance']
                    inherit_jobs = next(w for w in performance.winfo_children() if w.winfo_class() == 'TCheckbutton' and str(w.cget('variable')) == str(view['inherits']['jobs']))
                    jobs_entry = next(w for w in performance.winfo_children() if w.winfo_class() == 'TEntry' and str(w.cget('textvariable')) == str(view['variables']['jobs']))
                    inherit_jobs.invoke()
                    self.assertTrue(jobs_entry.instate(['disabled']))
                    reset_jobs = next(w for w in performance.winfo_children() if w.winfo_class() == 'TButton' and int(w.grid_info()['row']) == 1)
                    reset_jobs.invoke()
                    self.assertEqual(view['variables']['jobs'].get(), '8')
                    self.assertFalse(view['inherits']['jobs'].get())
                    self.assertFalse(jobs_entry.instate(['disabled']))
                    view['variables']['jobs'].set('4')
                    view['stage']('configure')
                    self.assertEqual(view['controller'].pending['values'], {'jobs':'4'})
                    self.assertEqual(str(view['tabs'].select()), str(view['review']))
                    view['back']()
                    self.assertIsNone(view['controller'].pending)
                    view['variables']['jobs'].set('0')
                    view['stage']('configure')
                    self.assertIn('positive integer', view['error'].get())
                    performance = view['pages']['Performance']
                    inherit_jobs = next(w for w in performance.winfo_children() if w.winfo_class() == 'TCheckbutton' and str(w.cget('variable')) == str(view['inherits']['jobs']))
                    jobs_entry = next(w for w in performance.winfo_children() if w.winfo_class() == 'TEntry' and str(w.cget('textvariable')) == str(view['variables']['jobs']))
                    inherit_jobs.invoke()
                    self.assertTrue(jobs_entry.instate(['disabled']))
                    reset_jobs = next(w for w in performance.winfo_children() if w.winfo_class() == 'TButton' and int(w.grid_info()['row']) == 1)
                    reset_jobs.invoke()
                    self.assertEqual(view['variables']['jobs'].get(), '8')
                    self.assertFalse(view['inherits']['jobs'].get())
                    self.assertFalse(jobs_entry.instate(['disabled']))
                    view['variables']['jobs'].set('4'); view['stage']('configure')
                    view['commit']()
                    wait(lambda: 'authentication cancelled' in view['error'].get())
                    self.assertIsNotNone(view['controller'].pending)
                    view['clear']()
                    view['table'].selection_set('cat/a'); root.update()
                    view['variables']['load'].set('1.5')
                    view['inherits']['load'].set(False)
                    # Invoke the actual inheritance toggle through its command.
                    def descendants(widget):
                        for child in widget.winfo_children():
                            yield child
                            yield from descendants(child)
                    inherit = next(w for w in descendants(root) if w.winfo_class() == 'TCheckbutton' and str(w.cget('variable')) == str(view['inherits']['load']))
                    inherit.invoke()
                    view['stage']('configure')
                    self.assertEqual(view['controller'].pending['values'], {})
                    view['back']()
                    view['variables']['root'].set('/etc/unsafe')
                    view['stage']('configure')
                    self.assertIn('workspace root', view['error'].get())
                    view['clear']()
                    view['operation']('list')
                    wait(lambda: workspace.called)
                    root.update()
                except Exception as exc:
                    failure.append(exc)
                finally:
                    for timer in root.tk.splitlist(root.tk.call('after', 'info')): root.after_cancel(timer)
                    root.destroy()
            with mock.patch.object(tk, 'Tk', return_value=root), mock.patch.object(root, 'mainloop', side_effect=interact), mock.patch.object(m, 'save', side_effect=RuntimeError('authentication cancelled')), mock.patch.object(m, 'workspace', return_value='[]') as workspace:
                m.gui(backend)
        if failure: raise failure[0]

    def test_adoption_save_and_workspace_prerequisites(self):
        root = tk.Tk()
        failures = []
        root.report_callback_exception = lambda *args: failures.append(args[1])
        with tempfile.TemporaryDirectory() as directory:
            backend = m.Backend(Path(directory), checks.FakePortage())
            (backend.root/'env').mkdir()
            (backend.root/'env/openmktr-ninja').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="ninja"\n')
            (backend.root/'package.env').write_text('cat/pkg openmktr-ninja\n')
            def interact():
                try:
                    view = root.manager
                    deadline = time.monotonic() + 8
                    while not view['table'].get_children():
                        if time.monotonic() > deadline: self.fail('discovery timed out')
                        root.update(); time.sleep(.02)
                    view['table'].selection_set('cat/pkg'); root.update()
                    self.assertEqual(view['variables']['mode'].get(), 'ninja')
                    view['stage']('adopt')
                    self.assertEqual(view['controller'].pending['values'], {})
                    view['commit']()
                    while backend.show('cat/pkg')['state'] != 'managed' or view['controller'].pending:
                        if time.monotonic() > deadline: self.fail('save timed out')
                        root.update(); time.sleep(.02)
                    for _ in range(5): root.update(); time.sleep(.03)
                    view['table'].selection_set('cat/pkg'); root.update()
                    workspace_toggle = next(w for w in view['pages']['Workspaces'].winfo_children() if w.winfo_class() == 'TCheckbutton')
                    workspace_toggle.invoke()
                    view['stage']('configure')
                    self.assertIn('setup-workspaces', view['error'].get())
                    self.assertIsNone(view['controller'].pending)
                    view['clear']()
                    view['stage']('disable')
                    self.assertEqual(view['controller'].pending['action'], 'disable')
                except Exception as exc: failures.append(exc)
                finally: root.destroy()
            with mock.patch.object(tk, 'Tk', return_value=root), mock.patch.object(root, 'mainloop', side_effect=interact), mock.patch.object(m, 'save', side_effect=lambda b, plan: b.apply(plan)):
                m.gui(backend)
        if failures: raise failures[0]

if __name__ == '__main__': unittest.main()
