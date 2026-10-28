#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Real widget checks with a private display and disposable configuration."""
import importlib.util
from pathlib import Path
import tempfile
import time
import threading
import unittest
from unittest import mock
import tkinter as tk

spec = importlib.util.spec_from_file_location('checks', Path(__file__).with_name('manager.py'))
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)
m = checks.m

class NativeTk(unittest.TestCase):
    def test_unadoptable_package_does_not_block_catalog(self):
        root = tk.Tk()
        failures = []
        root.report_callback_exception = lambda *args: failures.append(args[1])
        with tempfile.TemporaryDirectory() as directory:
            backend = m.Backend(Path(directory), checks.FakePortage())
            backend.portage.packages = lambda: [dict(package=p, installed=True, available=True) for p in ('cat/pkg','other/pkg')]
            (backend.root/'env').mkdir()
            (backend.root/'env/openmktr-ninja').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="ninja"\n')
            # Manual activation mode conflicts are selection-specific errors.
            (backend.root/'env/openmktr-direct').write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="direct"\n')
            (backend.root/'package.env').write_text('cat/pkg openmktr-ninja openmktr-direct\n')
            def interact():
                try:
                    deadline = time.monotonic()+5
                    view = root.manager
                    while len(view['table'].get_children()) != 2:
                        if time.monotonic()>deadline: self.fail('catalog failed to load')
                        root.update(); time.sleep(.01)
                    view['table'].selection_set('cat/pkg'); root.update()
                    self.assertIn('conflicting activation modes', view['hint'].get())
                    view['table'].selection_set('other/pkg'); root.update()
                    view['variables']['jobs'].set('4'); view['stage']('configure')
                    self.assertEqual(view['controller'].pending['packages'], ['other/pkg'])
                except Exception as exc: failures.append(exc)
                finally: root.destroy()
            with mock.patch.object(tk,'Tk',return_value=root), mock.patch.object(root,'mainloop',side_effect=interact):
                m.gui(backend)
        if failures: raise failures[0]

    def test_search_resize_and_cpu_shortcut(self):
        root = tk.Tk()
        failures = []
        root.report_callback_exception = lambda *args: failures.append(args[1])
        with tempfile.TemporaryDirectory() as directory:
            catalog = checks.FakePortage()
            release_catalog = threading.Event()
            def packages():
                if not release_catalog.wait(2): raise RuntimeError('GUI did not process timers during discovery')
                return [dict(package=f'cat/pkg{i}', installed=True, available=True) for i in range(1000)]
            catalog.packages = packages
            def children(widget):
                for child in widget.winfo_children():
                    yield child
                    yield from children(child)
            def wait(predicate):
                deadline = time.monotonic()+8
                while not predicate():
                    if time.monotonic()>deadline: self.fail('UI timed out')
                    root.update(); time.sleep(.01)
            def interact():
                try:
                    view = root.manager
                    root.after(50, release_catalog.set)
                    wait(lambda: len(view['table'].get_children()) == 1000)
                    buttons = {w.cget('text'):w for w in children(root) if w.winfo_class()=='TButton'}
                    root.geometry('640x480'); root.update()
                    self.assertEqual(root.minsize(), (640,480))
                    fullscreen_state = [False]
                    def attributes(*args):
                        if len(args)==1: return fullscreen_state[0]
                        fullscreen_state[0] = bool(args[1])
                    with mock.patch.object(root, 'attributes', side_effect=attributes):
                        buttons['Fullscreen (F11)'].invoke()
                        self.assertTrue(fullscreen_state[0])
                        root.event_generate('<Escape>'); root.update()
                        if fullscreen_state[0]: buttons['Fullscreen (F11)'].invoke()
                        self.assertFalse(fullscreen_state[0])
                    view['table'].selection_set('cat/pkg1'); root.update()
                    buttons['Use available CPUs'].invoke()
                    jobs = view['variables']['jobs'].get()
                    self.assertGreaterEqual(int(jobs), 1)
                    view['query'].set('pkg9'); buttons['Search'].invoke()
                    view['query'].set('missing'); buttons['Search'].invoke()
                    wait(lambda: not view['table'].get_children())
                    self.assertEqual(view['variables']['jobs'].get(), jobs)
                    buttons['Clear'].invoke()
                    wait(lambda: len(view['table'].get_children()) == 1000)
                    self.assertIn('cat/pkg1', view['table'].selection())
                    self.assertEqual(view['variables']['jobs'].get(), jobs)
                    with mock.patch.object(checks.m, 'workspace', side_effect=RuntimeError()):
                        view['operation']('list')
                        wait(lambda: 'RuntimeError: operation failed' in view['error'].get())
                    with mock.patch.object(checks.m, 'workspace', return_value=''):
                        view['operation']('list')
                        wait(lambda: any('No workspaces found.' in w.get('1.0','end') for w in children(root) if w.winfo_class()=='Text'))
                    view['clear']()
                    buttons['Refresh catalog'].invoke()
                    wait(lambda: len(view['table'].get_children()) == 1000)
                except Exception as exc: failures.append(exc)
                finally: root.destroy()
            with mock.patch.object(tk,'Tk',return_value=root), mock.patch.object(root,'mainloop',side_effect=interact):
                m.gui(m.Backend(Path(directory),catalog))
        if failures: raise failures[0]

    def test_editor_review_failure_batch_and_workspace(self):
        root = tk.Tk()
        failure = []
        release_save = threading.Event()
        def failed_save(*args):
            release_save.wait(5)
            raise RuntimeError('authentication cancelled')
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
                    release_workspace = threading.Event()
                    def slow_workspace(request):
                        release_workspace.wait(5)
                        return '[]'
                    with mock.patch.object(m, 'workspace', side_effect=slow_workspace):
                        view['operation']('list'); root.update()
                        self.assertTrue(inherit_jobs.instate(['disabled']))
                        inherit_jobs.invoke()
                        self.assertFalse(view['inherits']['jobs'].get())
                        release_workspace.set()
                        wait(lambda: not inherit_jobs.instate(['disabled']))
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
                    release_workspace = threading.Event()
                    def slow_workspace(request):
                        release_workspace.wait(5)
                        return '[]'
                    with mock.patch.object(m, 'workspace', side_effect=slow_workspace):
                        view['operation']('list'); root.update()
                        self.assertTrue(inherit_jobs.instate(['disabled']))
                        inherit_jobs.invoke()
                        self.assertFalse(view['inherits']['jobs'].get())
                        release_workspace.set()
                        wait(lambda: not inherit_jobs.instate(['disabled']))
                    inherit_jobs.invoke()
                    self.assertTrue(jobs_entry.instate(['disabled']))
                    reset_jobs = next(w for w in performance.winfo_children() if w.winfo_class() == 'TButton' and int(w.grid_info()['row']) == 1)
                    reset_jobs.invoke()
                    self.assertEqual(view['variables']['jobs'].get(), '8')
                    self.assertFalse(view['inherits']['jobs'].get())
                    self.assertFalse(jobs_entry.instate(['disabled']))
                    view['variables']['jobs'].set('4'); view['stage']('configure')
                    pending = view['controller'].pending
                    view['commit']()
                    view['tabs'].select(view['pages']['Build'].tab_container); root.update()
                    self.assertIs(view['controller'].pending, pending)
                    self.assertEqual(str(view['tabs'].select()), str(view['review']))
                    release_save.set()
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
                    view['workspace_vars']['profile'].set('existing-profile')
                    view['workspace_vars']['name'].set('cat/a:0:host')
                    for action in ('list', 'gc', 'status', 'pack', 'unpack'):
                        workspace.reset_mock()
                        view['operation'](action)
                        wait(lambda: workspace.called)
                        # Wait until the completion is delivered before the next action.
                        for _ in range(5): root.update(); time.sleep(.03)
                        request = workspace.call_args.args[0]
                        self.assertEqual(request['action'], action)
                        if action in ('list', 'gc'):
                            self.assertNotIn('profile', request)
                            self.assertNotIn('name', request)
                        else:
                            self.assertEqual(request['profile'], 'existing-profile')
                            self.assertEqual(request['name'], 'cat/a:0:host')
                        self.assertEqual('compression' in request, action in ('pack', 'gc'))
                        self.assertEqual('idle_seconds' in request, action == 'gc')
                except Exception as exc:
                    failure.append(exc)
                finally:
                    for timer in root.tk.splitlist(root.tk.call('after', 'info')): root.after_cancel(timer)
                    root.destroy()
            with mock.patch.object(tk, 'Tk', return_value=root), mock.patch.object(root, 'mainloop', side_effect=interact), mock.patch.object(m, 'save', side_effect=failed_save), mock.patch.object(m, 'workspace', return_value='[]') as workspace:
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
