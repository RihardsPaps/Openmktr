#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Measure manager discovery and filtered navigation without Portage or display."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import time

spec = importlib.util.spec_from_file_location('checks', Path(__file__).with_name('manager.py'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, help='Previous omktr_manager.py for paired comparison')
args = parser.parse_args()
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
with tempfile.TemporaryDirectory() as directory:
    backend = c.m.Backend(Path(directory), c.FakePortage())
    (backend.root / 'package.env').write_text(''.join(f'cat/pkg{i} custom\n' for i in range(100)))
    (backend.root / 'env').mkdir()
    (backend.root / 'env/custom').write_text('CFLAGS="-O2"\n')
    for count in (1000, 10000, 30000):
        rows = [dict(package=f'cat/pkg{i}', installed=True, available=True) for i in range(count)]
        start = time.perf_counter()
        snapshot = backend.snapshot()
        information = {r['package']: snapshot.show(r['package']) for r in rows}
        discovery = time.perf_counter()-start
        start = time.perf_counter()
        visible = [r['package'] for r in rows if r['installed']]
        filtering = time.perf_counter()-start
        assert len(visible) == count and len(information) == count
        result = dict(packages=count, discovery_seconds=discovery, installed_filter_seconds=filtering)
        if args.baseline:
            baseline_spec = importlib.util.spec_from_file_location('baseline', args.baseline)
            baseline = importlib.util.module_from_spec(baseline_spec)
            baseline_spec.loader.exec_module(baseline)
            old = baseline.Backend(backend.root, c.FakePortage())
            start = time.perf_counter()
            old_information = {r['package']: old.show(r['package']) for r in rows}
            result['baseline_discovery_seconds'] = time.perf_counter()-start
            start = time.perf_counter()
            old_visible = [p for p in old_information if next(r for r in rows if r['package']==p)['installed']]
            result['baseline_installed_filter_seconds'] = time.perf_counter()-start
            assert old_visible == visible
        print(json.dumps(result), flush=True)
