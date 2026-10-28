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

if __name__ == '__main__': unittest.main()
