#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Bounded compiler-cache investigation; requires a disposable Gentoo container.

Run fixtures first, then real ebuild upgrades. Never use this on a host system.
Compiler cache hits are evidence of reuse, not by themselves correctness proof.
"""

import argparse
from contextlib import contextmanager
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import statistics
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC = importlib.util.spec_from_file_location('portage_evidence', HERE / 'portage.py')
PORTAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PORTAGE)
PAIRS = {'gzip': ('app-arch/gzip', '1.14-r1', '1.15'),
         'xz-utils': ('app-arch/xz-utils', '5.8.3', '5.8.4')}
STATES = ('disabled', 'cold', 'upgrade')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require_equal(expected, actual, label):
    if expected != actual:
        keys = sorted(k for k in set(expected) | set(actual)
                      if expected.get(k) != actual.get(k))
        raise RuntimeError(f'{label}: uncached comparison mismatch: {keys[:20]}')


def restore_seed(seed, target):
    # Both are campaign-owned paths, never inferred from arbitrary package input.
    if target == seed or seed in target.parents or target in seed.parents:
        raise ValueError('seed and writable cache must be separate')
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(seed, target)


def counters(text):
    return {parts[0]: int(parts[1]) for line in text.splitlines()
            if len(parts := line.split()) == 2 and parts[1].isdigit()}


def counter_delta(before, after):
    return {k: after.get(k, 0) - before.get(k, 0) for k in set(before) | set(after)
            if k not in ('stats_updated_timestamp', 'stats_zeroed_timestamp')}


def summarize(samples):
    groups = {}
    for sample in samples:
        key = '/'.join(sample[k] for k in ('package', 'mode', 'profile', 'cache_state'))
        groups.setdefault(key, []).append(sample)
    summary = {}
    for key, rows in groups.items():
        summary[key] = {'samples': len(rows)}
        for metric in ('build_seconds', 'compile_seconds', 'cpu_seconds'):
            values = [row[metric] for row in rows]
            summary[key][metric] = {'median': statistics.median(values),
                                    'min': min(values), 'max': max(values)}
        summary[key]['cache_hits'] = [sum(v for k, v in r['cache_stats'].items()
                                         if k in ('direct_cache_hit', 'preprocessed_cache_hit'))
                                     for r in rows]
    return summary


def write_report(result, output):
    lines = ['# Cross-version compiler-cache investigation', '',
             'Measured configurations only. Correctness requires matching the uncached new state.', '',
             f'Controlled combinations passed: {len(result.get("fixtures", []))}.', '',
             '| Package / executor / paths | State | Samples | Build median (s) | Compile median (s) | Build change |',
             '| --- | --- | ---: | ---: | ---: | ---: |']
    summary = result.get('summary', {})
    for key, row in sorted(summary.items()):
        prefix, state = key.rsplit('/', 1)
        baseline = summary[prefix + '/disabled']['build_seconds']['median']
        change = (row['build_seconds']['median'] / baseline - 1) * 100
        lines.append(f'| {prefix} | {state} | {row["samples"]} | '
                     f'{row["build_seconds"]["median"]:.3f} | '
                     f'{row["compile_seconds"]["median"]:.3f} | {change:+.1f}% |')
    lines += ['', 'Positive changes mean longer builds. Configuration, generators, linking, tests,',
              'and staged installation always run. Downloads, dependency installation, cache copying,',
              'and final artifact/smoke verification are outside the build timer.', '',
              'See result.json, commands.jsonl, per-build compiler journals and test reports for evidence.',
              'A passing selected workload does not establish safety for arbitrary compiler inputs.', '']
    (output / 'report.md').write_text('\n'.join(lines))


class Campaign:
    def __init__(self, args, result):
        self.args, self.result, self.output = args, result, args.output
        self.work = Path('/var/tmp/openmktr-reuse')
        if self.work.exists():
            raise RuntimeError(f'refusing an existing workspace: {self.work}')
        self.work.mkdir(mode=0o755)
        self.binary = str(args.omktr.resolve())
        if not Path(self.binary).is_file():
            raise RuntimeError('the candidate Openmktr binary is missing')
        self.control = self.work / 'control.json'
        self.cache = self.work / 'cache'
        self.journal = self.work / 'journal'
        self.config = self.work / 'ccache.conf'
        self.ccache = shutil.which('ccache')
        self.cc = shutil.which('clang')
        self.cxx = shutil.which('clang++')
        if not all((self.ccache, self.cc, self.cxx)):
            raise RuntimeError('ccache, clang and clang++ must be installed before the campaign')
        self.config.write_text(
            f'cache_dir = {self.cache}\nmax_size = 2Gi\ndirect_mode = false\n'
            'depend_mode = false\ncompiler_check = content\n'
            'hash_dir = true\nhard_link = false\n')
        for language in ('cc', 'cxx'):
            wrapper = self.work / language
            wrapper.write_text('#!/bin/sh\nexec ' + shutil.which('python3') + ' ' +
                               str(HERE / 'cache_compiler.py') + ' ' +
                               str(self.control) + ' ' + language + ' "$@"\n')
            wrapper.chmod(0o755)
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.startswith(('CCACHE_', 'MAKE', 'KATI_', 'OMKTR_')):
                self.env.pop(key)
        self.env.update(CC=str(self.work / 'cc'), CXX=str(self.work / 'cxx'),
                        SOURCE_DATE_EPOCH='1700000000', LC_ALL='C', TZ='UTC')
        self.seq = 0

    def run(self, command, label, **kwargs):
        self.seq += 1
        label = f'{self.seq:05d}-{label}'
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.monotonic()
        PORTAGE.run(command, self.output, label, env=kwargs.pop('env', self.env), **kwargs)
        elapsed = time.monotonic() - start
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        return {'seconds': elapsed,
                'cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                'log': label + '.log'}

    def select(self, state, profile, maps=()):
        if self.journal.exists():
            shutil.rmtree(self.journal)
        self.journal.mkdir(mode=0o700)
        shutil.chown(self.journal, user='portage', group='portage')
        self.cache.mkdir(mode=0o700, exist_ok=True)
        # The compiler runs as Portage's unprivileged build user. The private
        # workspace has no untrusted writers outside this disposable container.
        shutil.chown(self.cache, user='portage', group='portage')
        for path in self.cache.rglob('*'):
            shutil.chown(path, user='portage', group='portage')
        self.control.write_text(json.dumps(
            {'cc': self.cc, 'cxx': self.cxx, 'ccache': self.ccache,
             'cache_state': state, 'profile': profile, 'maps': list(maps),
             'config': str(self.config), 'journal': str(self.journal)}))

    def stats(self):
        env = self.env | {'CCACHE_CONFIGPATH': str(self.config)}
        return counters(subprocess.check_output([self.ccache, '--print-stats'], env=env, text=True))

    def records(self):
        return [json.loads(p.read_text()) for p in sorted(self.journal.glob('*.json'))]

    def retain_journal(self, label):
        records = self.records()
        PORTAGE.atomic_json(self.output / (label + '-compiler.json'), records)
        return records

    def evidence(self):
        self.result['environment'] = {}
        for tool in (self.ccache, self.cc, self.cxx, self.binary, 'ninja-reference'):
            record = self.run([tool, '--version'], Path(tool).name + '-version')
            path = Path(tool) if '/' in tool else Path(shutil.which(tool))
            self.result['environment'][tool] = {'sha256': digest(path), 'version_log': record['log']}
        self.result['repository_timestamp'] = Path(
            '/var/db/repos/gentoo/metadata/timestamp.chk').read_text().strip()
        self.result['host'] = {'uname': platform.uname()._asdict(),
                               'cpu_affinity': sorted(os.sched_getaffinity(0)),
                               'cpuinfo': Path('/proc/cpuinfo').read_text(),
                               'meminfo': Path('/proc/meminfo').read_text()}
        self.result['container_limits'] = {name: Path('/sys/fs/cgroup', name).read_text()
                                            for name in ('cpu.max', 'memory.max')
                                            if Path('/sys/fs/cgroup', name).is_file()}
        self.result['configuration'] = self.config.read_text()
        self.result['defaults'] = {k: str(v) if isinstance(v, Path) else v
                                   for k, v in vars(self.args).items()}
        self.result['source_revision'] = subprocess.check_output(
            ['git', '-c', 'safe.directory=' + str(ROOT), 'rev-parse', 'HEAD'],
            cwd=ROOT, text=True).strip()
        self.result['source_files'] = {str(p.relative_to(ROOT)): digest(p) for p in
                                       (HERE / 'cross_version_cache.py',
                                        HERE / 'cache_compiler.py', HERE / 'cache_fixtures.py',
                                        HERE / 'portage.py', ROOT / 'packaging/gentoo/omktr-portage')}
        tooling = self.output / 'tooling'
        tooling.mkdir()
        for name in self.result['source_files']:
            target = tooling / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        version_log = self.result['environment'][self.ccache]['version_log']
        if (self.output / version_log).read_text().splitlines()[0] != 'ccache version 4.14':
            raise RuntimeError('this experiment is pinned to ccache 4.14')
        self.run(['emerge', '--info'], 'emerge-info')
        self.run(['env'], 'environment')
        self.run(['ccache', '--show-config'], 'qualified-ccache-config',
                 env=self.env | {'CCACHE_CONFIGPATH': str(self.config),
                                 'CCACHE_SLOPPINESS': '', 'CCACHE_BASEDIR': '',
                                 'CCACHE_REMOTE_STORAGE': ''})
        self.result['installed_packages'] = sorted(str(p.relative_to('/var/db/pkg')) for
                                                  p in Path('/var/db/pkg').glob('*/*')
                                                  if p.is_dir())
        PORTAGE.atomic_json(self.output / 'result.json', self.result)


def file_inventory(root):
    """Compare actual outputs, not Make timestamps or cache-reported success."""
    objects, artifacts, deps = {}, {}, {}
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.is_symlink() or '.openmktr' in path.parts:
            continue
        name = str(path.relative_to(root))
        if path.suffix == '.o':
            objects[name] = digest(path)
        elif path.suffix in ('.a', '.so'):
            artifacts[name] = digest(path)
        elif path.suffix in ('.d', '.Po', '.Plo'):
            deps[name] = path.read_text(errors='replace')
        else:
            with path.open('rb') as stream:
                if stream.read(4) == b'\x7fELF':
                    artifacts[name] = digest(path)
    return {'objects': objects, 'artifacts': artifacts, 'dependencies': deps}


def diagnose(campaign, pair):
    """Replay one matching object outside timing; preserve ccache's hashed inputs."""
    seeds = [s for s in campaign.result.get('seeds', []) if s['package'] == pair]
    upgrades = [s for s in campaign.result.get('samples', [])
                if s['package'] == pair and s['cache_state'] == 'upgrade']
    if not seeds or not upgrades:
        return
    newest = upgrades[-1]
    old = next(s for s in seeds if (s['mode'], s['profile']) ==
               (newest['mode'], newest['profile']))
    rows = []
    for sample in (old, newest):
        calls = json.loads((campaign.output / (sample['label'] + '-compiler.json')).read_text())
        rows.append([r for r in calls if r.get('phase') in (None, 'compile') and r['compile']
                     and r['exit_status'] == 0 and r.get('output') and
                     'conftest' not in r['output'] and r['cached']])
    common = sorted({Path(r['output']).name for r in rows[0]} &
                    {Path(r['output']).name for r in rows[1]})
    if not common:
        return
    texts = []
    for version, calls in zip(('old', 'new'), rows):
        call = next(r for r in calls if Path(r['output']).name == common[0])
        debug = campaign.output / f'{pair}-diagnostic-{version}'
        debug.mkdir()
        env = campaign.env | {'CCACHE_CONFIGPATH': str(campaign.config),
                              'CCACHE_SLOPPINESS': '', 'CCACHE_BASEDIR': '',
                              'CCACHE_REMOTE_STORAGE': '', 'CCACHE_RECACHE': 'true',
                              'CCACHE_DEBUG': 'true', 'CCACHE_DEBUGDIR': str(debug)}
        campaign.run(call['command'], f'{pair}-diagnostic-{version}', cwd=call['cwd'], env=env)
        files = list(debug.rglob('*.ccache-input-text'))
        if len(files) != 1:
            raise RuntimeError('expected one diagnostic ccache input record')
        texts.append(files[0].read_text(errors='replace').splitlines(keepends=True))
    (campaign.output / (pair + '-cache-input.diff')).write_text(''.join(
        difflib.unified_diff(*texts, fromfile='older version', tofile='newer version')))


@contextmanager
def package_settings(campaign):
    config = Path('/etc/portage')
    cleanup = []
    selections = ''.join(f'{package} openmktr-reuse\n' for package, _, _ in PAIRS.values())
    try:
        PORTAGE.write_new(config / 'package.env/openmktr-reuse', selections, cleanup)
        PORTAGE.write_new(config / 'package.accept_keywords/openmktr-reuse',
                          ''.join(f'={p}-{v} ~amd64\n' for p, old, new in PAIRS.values()
                                  for v in (old, new)), cleanup)
        PORTAGE.write_new(config / 'package.use/openmktr-reuse',
                          'app-arch/gzip -static pic\n'
                          'app-arch/xz-utils -pgo -doc -nls -static-libs abi_x86_64 -abi_x86_32\n',
                          cleanup)
        envfile = config / 'env/openmktr-reuse'
        PORTAGE.write_new(envfile, '', cleanup)
        yield envfile
    finally:
        for path in reversed(cleanup):
            path.unlink()


def real_packages(campaign):
    args, result, output = campaign.args, campaign.result, campaign.output
    repo = Path('/var/db/repos/gentoo')
    # Install the explicitly selected candidate binary and the unchanged adapter.
    for name, source in (('omktr', Path(campaign.binary)),
                         ('omktr-portage', ROOT / 'packaging/gentoo/omktr-portage')):
        target = Path('/usr/bin') / name
        if target.exists():
            raise RuntimeError(f'refusing to replace {target}')
        shutil.copyfile(source, target)
        target.chmod(0o755)
    try:
        with package_settings(campaign) as envfile:
            for pair in args.pair:
                package, old, new = PAIRS[pair]
                ebuilds = [repo / package / f'{pair}-{v}.ebuild' for v in (old, new)]
                if not all(p.is_file() for p in ebuilds):
                    raise RuntimeError('pinned ebuild missing; do not silently substitute versions')
                provenance = {str(p.relative_to(repo)): digest(p) for directory in
                              (repo / package, repo / 'eclass') for p in directory.rglob('*')
                              if p.is_file()}
                PORTAGE.atomic_json(output / (pair + '-repository-inputs.json'), provenance)
                campaign.run(['emerge', '--oneshot', '--onlydeps', '--getbinpkg',
                              '--autounmask=n', *['=' + package + '-' + v for v in (old, new)]],
                             pair + '-dependencies')
                for ebuild in ebuilds:
                    campaign.select('disabled', 'original')
                    campaign.run(['ebuild', ebuild, 'fetch'], pair + '-fetch')
                distdir = Path(subprocess.check_output(['portageq', 'envvar', 'DISTDIR'], text=True).strip())
                archives = {str(distdir / words[1]): digest(distdir / words[1])
                            for line in (repo / package / 'Manifest').read_text().splitlines()
                            if (words := line.split()) and words[0] == 'DIST' and
                            (distdir / words[1]).is_file()}
                PORTAGE.atomic_json(output / (pair + '-archives.json'), archives)
                for mode in args.mode:
                    for profile in args.profile:
                        seed = campaign.work / f'seed-{pair}-{mode}-{profile}'
                        if campaign.cache.exists():
                            shutil.rmtree(campaign.cache)
                        sample = build_package(campaign, envfile, pair, old, mode,
                                               profile, 'cold', 'seed')
                        shutil.copytree(campaign.cache, seed)
                        result.setdefault('seeds', []).append(sample)
                        for repeat in range(args.repeats):
                            states = STATES if repeat % 2 == 0 else tuple(reversed(STATES))
                            pending = []
                            for state in states:
                                if state == 'upgrade':
                                    restore_seed(seed, campaign.cache)
                                elif campaign.cache.exists():
                                    shutil.rmtree(campaign.cache)
                                pending.append(build_package(campaign, envfile, pair, new,
                                                             mode, profile, state, str(repeat)))
                            reference = next(p for p in pending if p['cache_state'] == 'disabled')
                            for sample in pending:
                                for kind in ('objects', 'artifacts', 'dependencies'):
                                    require_equal(reference['compile_inventory'][kind],
                                                  sample['compile_inventory'][kind],
                                                  sample['label'] + ' compile ' + kind)
                                    require_equal(reference['inventory'][kind],
                                                  sample['inventory'][kind], sample['label'] + kind)
                                require_equal(reference['staged_artifacts'], sample['staged_artifacts'],
                                              sample['label'] + ' staged artifacts')
                                if reference['diagnostics'] != sample['diagnostics']:
                                    raise RuntimeError(sample['label'] + ': compiler diagnostics mismatch')
                                sample['verified'] = True
                                PORTAGE.atomic_json(output / sample['label'] / 'sample.json', sample)
                            result.setdefault('samples', []).extend(pending)
                            result['summary'] = summarize(result['samples'])
                            PORTAGE.atomic_json(output / 'result.json', result)
                        shutil.rmtree(seed)
                require_equal(provenance, {name: digest(repo / name) for name in provenance},
                              pair + ' repository integrity')
                require_equal(archives, {name: digest(name) for name in archives},
                              pair + ' archive integrity')
                diagnose(campaign, pair)
    finally:
        for name in ('omktr', 'omktr-portage'):
            (Path('/usr/bin') / name).unlink(missing_ok=True)


def build_package(campaign, envfile, pair, version, mode, profile, state, repeat):
    try:
        return _build_package(campaign, envfile, pair, version, mode, profile, state, repeat)
    except BaseException:
        label = f'{pair}-{version}-{mode}-{profile}-{state}-{repeat}-failure'
        directory = campaign.output / label
        directory.mkdir(exist_ok=True)
        base = Path('/var/tmp/portage') / PAIRS[pair][0].split('/')[0] / f'{pair}-{version}'
        for path in base.rglob('*'):
            if path.is_file() and (path.name in ('config.log', 'build.log', 'environment',
                                                'test-suite.log', 'build.ninja') or path.suffix == '.trs'):
                target = directory / path.relative_to(base)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        campaign.retain_journal(label)
        raise


def _build_package(campaign, envfile, pair, version, mode, profile, state, repeat):
    package = PAIRS[pair][0]
    pf = f'{pair}-{version}'
    ebuild = Path('/var/db/repos/gentoo') / package / (pf + '.ebuild')
    base = Path('/var/tmp/portage') / package.split('/')[0] / pf
    work = base / 'work'
    source = work / (('xz-' if pair == 'xz-utils' else 'gzip-') + version.split('-r')[0])
    maps = [(str(work), '/openmktr-work'), (str(source), '/openmktr-src')]
    if pair == 'xz-utils':
        maps.append((str(source) + '-abi_x86_64', '/openmktr-build'))
    campaign.select(state, profile, maps)
    label = f'{pair}-{version}-{mode}-{profile}-{state}-{repeat}'
    settings = (f'export MAKE="/usr/bin/omktr-portage"\n'
                f'export OMKTR_PORTAGE_MODE="{mode}"\n'
                f'CC="{campaign.work}/cc"\nCXX="{campaign.work}/cxx"\n'
                'CFLAGS="-O2 -g"\nCXXFLAGS="-O2 -g"\nLDFLAGS="-fuse-ld=lld"\n'
                f'MAKEOPTS="-j{campaign.args.jobs} -l1000000"\n'
                'export SOURCE_DATE_EPOCH="1700000000"\n'
                f'FEATURES="${{FEATURES}} test keepwork keeptemp -ccache '
                f'{PORTAGE.NAMESPACE_FEATURES}"\n')
    envfile.write_text(settings + 'FEATURES="${FEATURES} -keepwork -keeptemp"\n')
    campaign.run(['ebuild', ebuild, 'clean'], label + '-clean')
    envfile.write_text(settings)
    if work.exists():
        raise RuntimeError('clean left a work tree; cannot measure fresh builds')
    before = campaign.stats()
    phases = {}
    started = time.monotonic()
    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    for phase in ('unpack', 'prepare', 'configure', 'compile', 'test', 'install'):
        phase_before = campaign.stats()
        phases[phase] = campaign.run(['ebuild', ebuild, phase], label + '-' + phase)
        phases[phase]['cache_stats'] = counter_delta(phase_before, campaign.stats())
        if phase == 'compile':
            compile_inventory = file_inventory(work)
            compile_records = campaign.records()
    build_seconds = time.monotonic() - started
    cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    final_inventory = file_inventory(work)
    after = campaign.stats()
    records = campaign.retain_journal(label)
    for phase, measurement in phases.items():
        calls = [r for r in records if r['phase'] == phase]
        measurement['compiler_calls'] = sum(r['compile'] for r in calls)
        measurement['link_calls'] = sum(not r['compile'] and not r['preprocess'] and
                                        '-o' in r['command'] for r in calls)
    if not compile_inventory['objects'] or not compile_inventory['artifacts']:
        raise RuntimeError(label + ': no compiled artifacts')
    logs = '\n'.join((campaign.output / p['log']).read_text(errors='replace')
                     for p in phases.values())
    PORTAGE.verify_real_phases(logs, label)
    tests = sum(int(n) for p in work.rglob('test-suite.log') for n in
                re.findall(r'^# TOTAL:\s*(\d+)', p.read_text(errors='replace'), re.M))
    if tests == 0:
        raise RuntimeError(label + ': no nonempty upstream test report')
    graphs = list(work.rglob('.openmktr/build.ninja'))
    if mode == 'ninja' and not graphs:
        raise RuntimeError(label + ': Ninja compilation was not exercised')
    image = base / 'image'
    if pair == 'gzip':
        binary = next(image.rglob('gzip-reference'))
    else:
        binary = image / 'usr/bin/xz'
    smoke_env = campaign.env | {'LD_LIBRARY_PATH': str(image / 'usr/lib64') + ':' +
                               str(image / 'usr/lib')}
    payload = b'Openmktr cross-version cache round trip\n' * 100
    compressed = subprocess.check_output([binary, '-c'], input=payload, env=smoke_env)
    if subprocess.check_output([binary, '-dc'], input=compressed, env=smoke_env) != payload:
        raise RuntimeError(label + ': compression round trip failed')
    if pair == 'xz-utils':
        probe = campaign.work / 'lzma-probe.c'
        probe.write_text('#include <lzma.h>\n#include <stdio.h>\n'
                         'int main(void) { puts(lzma_version_string()); return 0; }\n')
        campaign.run([campaign.cc, probe, '-I' + str(image / 'usr/include'),
                      '-L' + str(image / 'usr/lib64'), '-Wl,-rpath,' + str(image / 'usr/lib64'),
                      '-llzma', '-o', campaign.work / 'lzma-probe'], label + '-client-build')
        smoke = campaign.run([campaign.work / 'lzma-probe'], label + '-client')
        if (campaign.output / smoke['log']).read_text().strip() != version:
            raise RuntimeError(label + ': client did not use newly built liblzma')
    retained = campaign.output / label
    retained.mkdir()
    for path in work.rglob('*'):
        if path.is_file() and (path.name in ('config.log', 'test-suite.log', 'build.ninja')
                               or path.suffix == '.trs'):
            destination = retained / path.relative_to(work)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
    sample = {'label': label, 'package': pair, 'version': version, 'mode': mode,
              'profile': profile, 'cache_state': state, 'repeat': repeat,
              'phases': phases, 'compile_seconds': phases['compile']['seconds'],
              'build_seconds': build_seconds,
              'phase_total_seconds': sum(p['seconds'] for p in phases.values()),
              'cpu_seconds': cpu_after.ru_utime + cpu_after.ru_stime - cpu_before.ru_utime - cpu_before.ru_stime,
              'compiler_calls': sum(r['compile'] for r in records),
              'link_calls': sum(not r['compile'] and not r['preprocess'] and
                                '-o' in r['command'] for r in records),
              'unsupported_calls': sum(r['unsupported'] for r in records),
              'cache_bytes': sum(p.stat().st_size for p in campaign.cache.rglob('*') if p.is_file()),
              'cache_stats': counter_delta(before, after), 'upstream_tests': tests,
              'smoke': 'passed', 'inventory': final_inventory,
              'compile_inventory': compile_inventory,
              'staged_artifacts': file_inventory(image)['artifacts'],
              'diagnostics': sorted((r.get('output', ''), r['stdout'], r['stderr'])
                                    for r in records if r.get('phase') in ('compile', 'test') and r['compile'] and
                                    (r['stdout'] or r['stderr']))}
    PORTAGE.atomic_json(retained / 'sample.json', sample)
    return sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--omktr', type=Path, default=Path('/workspace/omktr'))
    parser.add_argument('--pair', action='append', choices=PAIRS)
    parser.add_argument('--mode', action='append', choices=('direct', 'ninja'))
    parser.add_argument('--profile', action='append', choices=('original', 'normalized'))
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--fixtures-only', action='store_true')
    parser.add_argument('--packages-only', action='store_true')
    parser.add_argument('--fixture', action='append', help='select a controlled fixture (repeatable)')
    args = parser.parse_args()
    if (os.environ.get('OMKTR_PORTAGE_VALIDATION') != '1' or os.geteuid() != 0 or
            not Path('/etc/gentoo-release').exists()):
        parser.error('requires a disposable Gentoo container and OMKTR_PORTAGE_VALIDATION=1')
    if args.jobs < 1 or args.repeats < 1 or (args.fixtures_only and args.packages_only):
        parser.error('jobs/repeats must be positive; fixture/package selections are exclusive')
    args.output = args.output.resolve()
    args.pair = args.pair or list(PAIRS)
    args.mode = args.mode or ['direct', 'ninja']
    args.profile = args.profile or ['original', 'normalized']
    with PORTAGE.evidence_run(args.output, openmktr_use_test=False) as result:
        campaign = Campaign(args, result)
        campaign.evidence()
        if not args.packages_only:
            from cache_fixtures import fixtures
            fixtures(campaign)
        if not args.fixtures_only:
            real_packages(campaign)
        result['coverage'] = 'listed configurations only; no universal cache-safety claim'
        write_report(result, args.output)


if __name__ == '__main__':
    main()
