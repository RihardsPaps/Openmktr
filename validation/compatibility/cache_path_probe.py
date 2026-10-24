#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Bounded xz path probe using retained, configured trees from the first campaign.

Run in a disposable copy of that campaign container. No release sources are
edited. A miss is an experimental outcome, not a qualification success.
"""

import argparse
import copy
import difflib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from cross_version_cache import PORTAGE, counter_delta, counters, digest, restore_seed


OBJECTS = ('alone_decoder', 'vli_decoder', 'crc32_fast')


def dependency_paths(text, cwd):
    """Resolve emitted dependencies as the consuming build tool would."""
    body = text.replace('\\\n', '').split(':', 1)[1]
    return sorted({str((cwd / name).resolve()) for name in shlex.split(body)})


def qualifies(rows):
    # Same-directory replay hits only establish that the cache was populated.
    return (bool(rows) and all(r['object_equal'] and r['diagnostics_equal'] and
                r['dependencies_equal'] for r in rows) and
            any(r['upgrade_stats'].get('preprocessed_cache_hit', 0) > 0 for r in rows))


class Probe:
    def __init__(self, args, result):
        self.args, self.result = args, result
        self.work = Path('/var/tmp') / ('openmktr-path-probe-' + args.output.name)
        self.work.mkdir()  # An existing workspace is never reused.
        self.config = self.work / 'ccache.conf'
        self.cache = self.work / 'cache'
        self.config.write_text(
            f'cache_dir = {self.cache}\nmax_size = 2Gi\ndirect_mode = false\n'
            'depend_mode = false\ncompiler_check = content\n'
            'hash_dir = true\nhard_link = false\n')
        self.ccache = '/usr/bin/ccache'
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('CCACHE_')}
        self.env.update(CCACHE_CONFIGPATH=str(self.config), CCACHE_SLOPPINESS='',
                        CCACHE_REMOTE_STORAGE='', SOURCE_DATE_EPOCH='1700000000',
                        LC_ALL='C', TZ='UTC')
        self.sequence = 0
        self.inputs = {}

    def run(self, command, label, cwd=None, env=None):
        self.sequence += 1
        name = f'{self.sequence:04d}-{label}'
        PORTAGE.run(command, self.args.output, name, cwd=cwd, env=env or self.env,
                    timeout=120)
        return self.args.output / (name + '.log')

    def stats(self):
        return counters(subprocess.check_output(
            [self.ccache, '--print-stats'], env=self.env, text=True))

    def call(self, record, label, cached, basedir, depfiles):
        command = record['command'][1:]
        cwd = Path(record['cwd'])
        output = Path(record['output'])
        depfile = output.with_suffix('.path-probe.d')
        if depfiles:
            command += ['-MD', '-MF', str(depfile), '-MT', output.name]
        source = next(Path(a) for a in command if a.endswith('.c') and Path(a).is_file())
        self.inputs[str(source)] = digest(source)
        self.inputs[str(cwd / '../../config.h')] = digest(cwd / '../../config.h')
        output.unlink(missing_ok=True)
        depfile.unlink(missing_ok=True)
        debug = self.args.output / (label + '-debug')
        debug.mkdir()
        env = self.env | {'CCACHE_BASEDIR': basedir, 'CCACHE_DEBUG': 'true',
                          'CCACHE_DEBUGDIR': str(debug)}
        before = self.stats()
        log = self.run(([self.ccache] if cached else []) + command, label, cwd, env)
        stats = counter_delta(before, self.stats())
        dependencies = dependency_paths(depfile.read_text(), cwd) if depfiles else []
        if any(not Path(p).is_file() for p in dependencies):
            raise RuntimeError(label + ': depfile refers to a missing input')
        row = {'sha256': digest(output), 'diagnostics': log.read_text(),
               'dependencies': dependencies, 'cache_stats': stats,
               'command': command, 'cwd': str(cwd), 'base_dir': basedir}
        shutil.copyfile(output, self.args.output / (label + '.o'))
        if depfiles:
            (self.args.output / (label + '.d')).write_text(depfile.read_text())
        PORTAGE.atomic_json(self.args.output / (label + '.json'), row)
        return row

    def execute(self):
        prior = self.args.prior
        previous = json.loads((prior / 'result.json').read_text())
        if previous['status'] != 'passed':
            raise RuntimeError('prior campaign must have completed successfully')
        selected = [next(r for r in reversed(previous[group]) if r['package'] == 'xz-utils' and
                         r['mode'] == 'ninja' and r['profile'] == 'normalized' and
                         r['version'] == version and r['cache_state'] == state)
                    for group, version, state in (('seeds', '5.8.3', 'cold'),
                                                  ('samples', '5.8.4', 'upgrade'))]
        paths = [prior / (r['label'] + '-compiler.json') for r in selected]
        journals = [json.loads(p.read_text()) for p in paths]
        self.result.update(prior_result_sha256=digest(prior / 'result.json'),
                           journal_sha256={str(p): digest(p) for p in paths},
                           probe_sha256=digest(__file__), configuration=self.config.read_text(),
                           workload='relocation-only-old-content' if self.args.relocation_only else 'real-upgrade',
                           rows=[], summaries={})
        if self.args.relocation_only:
            journals = self.relocate(journals[0])
        tooling = self.args.output / 'tooling'
        tooling.mkdir()
        for name in ('cache_path_probe.py', 'cross_version_cache.py', 'portage.py'):
            shutil.copyfile(Path(__file__).with_name(name), tooling / name)
        for tool in (self.ccache, '/usr/lib/llvm/22/bin/clang', '/runtime/omktr'):
            self.run([tool, '--version'], Path(tool).name + '-version')
        if subprocess.check_output([self.ccache, '--version'], text=True).splitlines()[0] != 'ccache version 4.14':
            raise RuntimeError('requires the original pinned ccache 4.14')
        for profile in ('prefix-maps-only', 'common-basedir', 'work-basedir'):
            rows = []
            for name in OBJECTS:
                calls = [next(r for r in records if r.get('phase') == 'compile' and
                              r['compile'] and r.get('output', '').endswith(
                                  'liblzma_la-' + name + '.o')) for records in journals]
                for depfiles in (False, True):
                    label = f'{profile}-{name}-deps{int(depfiles)}'
                    if self.cache.exists():
                        shutil.rmtree(self.cache)
                    base_dirs = ['' if profile == 'prefix-maps-only' else
                                 str(Path(call['cwd']).parents[4]) if profile == 'common-basedir' else
                                 str(Path(call['cwd']).parents[2]) for call in calls]
                    oracle = self.call(calls[1], label + '-oracle', False, '', depfiles)
                    seeded = self.call(calls[0], label + '-seed', True, base_dirs[0], depfiles)
                    seed = self.work / 'seed'
                    if seed.exists():
                        shutil.rmtree(seed)
                    shutil.copytree(self.cache, seed)
                    repeat = self.call(calls[0], label + '-same-directory', True,
                                       base_dirs[0], depfiles)
                    if repeat['cache_stats'].get('preprocessed_cache_hit') != 1:
                        raise RuntimeError(label + ': positive control did not hit')
                    for key in ('sha256', 'dependencies', 'diagnostics'):
                        if seeded[key] != repeat[key]:
                            raise RuntimeError(label + ': same-directory hit changed ' + key)
                    restore_seed(seed, self.cache)
                    actual = self.call(calls[1], label + '-upgrade', True,
                                       base_dirs[1], depfiles)
                    debug_text = []
                    for state in ('seed', 'upgrade'):
                        files = list((self.args.output / (label + '-' + state + '-debug')).rglob(
                            '*.ccache-input-text'))
                        if len(files) != 1:
                            raise RuntimeError(label + ': missing ccache input evidence')
                        debug_text.append(files[0].read_text().splitlines(keepends=True))
                    (self.args.output / (label + '.diff')).write_text(''.join(
                        difflib.unified_diff(*debug_text, fromfile='old', tofile='new')))
                    row = {'profile': profile, 'object': name, 'depfiles': depfiles,
                           'object_equal': oracle['sha256'] == actual['sha256'],
                           'diagnostics_equal': oracle['diagnostics'] == actual['diagnostics'],
                           'dependencies_equal': oracle['dependencies'] == actual['dependencies'],
                           'upgrade_stats': actual['cache_stats'],
                           'same_directory_stats': repeat['cache_stats'],
                           'oracle_sha256': oracle['sha256'], 'cached_sha256': actual['sha256']}
                    rows.append(row)
                    self.result['rows'].append(row)
                    PORTAGE.atomic_json(self.args.output / 'result.json', self.result)
            self.result['summaries'][profile] = {'qualified': qualifies(rows),
                'upgrade_hits': sum(r['upgrade_stats'].get('preprocessed_cache_hit', 0) for r in rows),
                'comparisons': len(rows), 'mismatches': sum(not all(r[k] for k in
                    ('object_equal', 'diagnostics_equal', 'dependencies_equal')) for r in rows)}
        self.result['input_sha256'] = self.inputs
        if any(digest(name) != value for name, value in self.inputs.items()):
            raise RuntimeError('probe modified release sources or generated configuration')
        self.triage()
        success = any(v['qualified'] for v in self.result['summaries'].values())
        self.result['next_step'] = ('qualify incremental path fixtures before C++ measurement' if success else
                                    'stop: no correctly restored cross-version xz objects')
        print(json.dumps(self.result['summaries'], indent=2), flush=True)

    def relocate(self, records):
        """A labelled control: identical old content under both directory layouts."""
        original = Path('/var/tmp/portage/app-arch/xz-utils-5.8.3/work')
        relocated = []
        for version in ('5.8.3', '5.8.4'):
            target = self.work / 'relocation/app-arch' / ('xz-utils-' + version) / 'work'
            target.mkdir(parents=True)
            for source in original.iterdir():
                if source.is_dir() and source.name.startswith('xz-5.8.3'):
                    shutil.copytree(source, target / source.name.replace('5.8.3', version),
                                    symlinks=True)
            calls = copy.deepcopy(records)
            for call in calls:
                for key in ('cwd', 'output'):
                    if call.get(key):
                        call[key] = call[key].replace(str(original), str(target)).replace('xz-5.8.3', 'xz-' + version)
                call['command'] = [s.replace(str(original), str(target)).replace('xz-5.8.3', 'xz-' + version)
                                   for s in call['command']]
            relocated.append(calls)
        return relocated

    def triage(self):
        """Explain differences without weakening the exact-output requirement."""
        toolroot = Path('/usr/lib/llvm/22/bin')
        self.result['object_diagnostics'] = []
        for row in self.result['rows']:
            if row['object_equal']:
                continue
            label = f'{row["profile"]}-{row["object"]}-deps{int(row["depfiles"])}'
            stripped = []
            for state in ('oracle', 'upgrade'):
                obj = self.args.output / (label + '-' + state + '.o')
                self.run([toolroot / 'llvm-dwarfdump', '--debug-info', obj],
                         label + '-' + state + '-dwarf')
                dest = self.work / (state + '-stripped.o')
                self.run([toolroot / 'llvm-objcopy', '--strip-debug', obj, dest],
                         label + '-' + state + '-strip-debug')
                stripped.append(digest(dest))
            self.result['object_diagnostics'].append(
                {'label': label, 'equal_after_strip_debug': stripped[0] == stripped[1]})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prior', type=Path, required=True)
    parser.add_argument('--relocation-only', action='store_true',
                        help='control with identical old content copied into both versioned layouts')
    args = parser.parse_args()
    if (os.geteuid() != 0 or os.environ.get('OMKTR_PORTAGE_VALIDATION') != '1' or
            not Path('/etc/gentoo-release').exists()):
        parser.error('requires a disposable copy of the original Gentoo campaign container')
    args.output = args.output.resolve()
    with PORTAGE.evidence_run(args.output, openmktr_use_test=False) as result:
        Probe(args, result).execute()


if __name__ == '__main__':
    main()
