#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Exercise the overlay and EAPI 8 lifecycle in a disposable Gentoo container.

Run only in the optional Gentoo image. This intentionally installs and unmerges
packages and temporarily configures Portage; it is not a host diagnostic.
"""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
PACKAGES = ('openmktr-portage-fixture', 'openmktr-portage-control')
NAMESPACE_FEATURES = 'ipc-sandbox network-sandbox pid-sandbox'
REAL_PACKAGES = ('=app-misc/hello-2.12.2', '=dev-libs/libpipeline-1.5.8',
                 '=app-arch/gzip-1.14-r1')


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def namespace_warning(log):
    # Tests and echoed shell commands may quote these diagnostics. Match the
    # actual Portage diagnostic line, including its dependency-progress prefix.
    plain = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', log)
    return re.search(r'(?m)^(?:\s*\*?\s*|Calculating dependencies \.\.\.)'
                     r'Unable to (?:unshare|configure loopback interface|mount new /proc):',
                     plain) is not None


@contextmanager
def evidence_run(output, *, openmktr_use_test=True):
    """An evidence directory belongs to exactly one run, including failures."""
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f'evidence directory must be empty: {output}')
    result = {'status': 'running', 'started': datetime.now(timezone.utc).isoformat(),
              'image_id': os.environ.get('OMKTR_VALIDATION_IMAGE_ID') or None,
              'namespace_sandboxes': NAMESPACE_FEATURES.split(),
              'openmktr_use_test': openmktr_use_test}
    atomic_json(output / 'result.json', result)
    try:
        yield result
    except BaseException as error:
        result.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    else:
        result['status'] = 'passed'
    finally:
        result['finished'] = datetime.now(timezone.utc).isoformat()
        atomic_json(output / 'result.json', result)


def run(args, output, name, *, cwd=None, env=None, success=True, timeout=3600):
    started = time.monotonic()
    record = {'command': list(map(str, args)), 'exit_status': None,
              'log': name + '.log', 'expected_success': success, 'timed_out': False}
    try:
        with (output / (name + '.log')).open('x') as log:
            print('+', ' '.join(map(str, args)), flush=True)
            with subprocess.Popen(list(map(str, args)), cwd=cwd, env=env,
                                  stdout=log, stderr=subprocess.STDOUT,
                                  start_new_session=True) as process:
                try:
                    record['exit_status'] = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    record['timed_out'] = True
                    os.killpg(process.pid, signal.SIGKILL)
                    record['exit_status'] = process.wait()
                    raise
    except BaseException as error:
        record['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        record['seconds'] = time.monotonic() - started
        with (output / 'commands.jsonl').open('a') as stream:
            stream.write(json.dumps(record) + '\n')
    log_text = (output / (name + '.log')).read_text(errors='replace')
    if namespace_warning(log_text):
        raise RuntimeError('Portage namespace sandboxes could not start; run the '
                           'container with --cap-add SYS_ADMIN --cap-add NET_ADMIN --cap-add SYS_PTRACE '
                           '--security-opt seccomp=unconfined '
                           '--security-opt apparmor=unconfined; see ' + str(output))
    if (record['exit_status'] == 0) != success:
        raise RuntimeError(f'{name}: unexpected exit {record["exit_status"]}; see {output}')


def write_new(path, content, cleanup):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x') as stream:
        stream.write(content)
    cleanup.append(path)


def verify_real_phases(log, label):
    if any(marker not in log for marker in (
            '>>> Source configured.', '>>> Compiling source', '>>> Source compiled.',
            '>>> Test phase:', '>>> Completed testing', '>>> Completed installing')):
        raise RuntimeError(f'{label}: a required build phase was skipped')
    compile_log = log.split('>>> Compiling source', 1)[1].split('>>> Source compiled.', 1)[0]
    if '/usr/bin/omktr-portage' not in compile_log:
        raise RuntimeError(f'{label}: compile did not exercise the adapter')


def qualify_real_packages(atoms, output, config, cleanup, result):
    """Use unmodified repository ebuilds; retain their actual test reports."""
    selections = {atom: atom[1:].rsplit('-', 1)[0] for atom in atoms}
    # Revision suffixes are part of the version, not the package name.
    selections['=app-arch/gzip-1.14-r1'] = 'app-arch/gzip'
    write_new(config / 'package.use/openmktr-validation-real',
              'dev-libs/libpipeline test\n', cleanup)
    run(['emerge', '--oneshot', '--verbose', '--color=n', '--onlydeps',
         '--getbinpkg', '--autounmask=n', *atoms], output, 'real-dependencies')
    envfile = config / 'env/openmktr-validation-real'
    write_new(envfile, '', cleanup)
    write_new(config / 'package.env/openmktr-validation-real', ''.join(
        f'{selections[atom]} openmktr-validation-real\n' for atom in atoms), cleanup)
    result['real_packages'] = []
    for mode in ('direct', 'ninja'):
        envfile.write_text(
            'export MAKE="/usr/bin/omktr-portage"\n'
            f'export OMKTR_PORTAGE_MODE="{mode}"\n'
            'CC="clang"\nCXX="clang++"\nCFLAGS="-O1"\nCXXFLAGS="-O1"\n'
            'MAKEOPTS="-j4 -l1000000"\n'
            f'FEATURES="${{FEATURES}} test keepwork keeptemp {NAMESPACE_FEATURES}"\n')
        for atom in atoms:
            category, package = selections[atom].split('/')
            pf = atom.split('/')[1]
            label = f'real-{package}-{mode}'
            # keepwork retains completion stamps as well as sources. Clear
            # them through Portage before each mode so neither compile nor
            # upstream tests can be silently reused from an earlier emerge.
            ebuild = Path('/var/db/repos/gentoo') / category / package / (pf + '.ebuild')
            settings = envfile.read_text()
            envfile.write_text(settings + 'FEATURES="${FEATURES} -keepwork -keeptemp"\n')
            try:
                run(['ebuild', ebuild, 'clean'], output, label + '-clean')
            finally:
                envfile.write_text(settings)
            run(['emerge', '--oneshot', '--verbose', '--color=n', '--usepkg=n',
                 '--autounmask=n', atom], output, label + '-merge')
            log = (output / (label + '-merge.log')).read_text(errors='replace')
            verify_real_phases(log, label)
            evidence = output / label
            evidence.mkdir()
            vdb = Path('/var/db/pkg') / category / pf
            shutil.copytree(vdb, evidence / 'installed-package')
            build = Path('/var/tmp/portage') / category / pf / 'work'
            if not build.is_dir():
                raise RuntimeError(f'{label}: Portage did not retain the build tree')
            retained = []
            test_totals = []
            automake = []
            ninja_graphs = []
            for path in build.rglob('*'):
                if not path.is_file() or path.is_symlink():
                    continue
                if (path.name in ('Makefile', 'config.log', 'config.status',
                                  'build.ninja', 'test-suite.log')
                        or path.suffix in ('.log', '.trs')):
                    relative = path.relative_to(build)
                    target = evidence / 'build' / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target)
                    retained.append(str(relative))
                    if path.name == 'test-suite.log':
                        test_totals.extend(int(value) for value in re.findall(
                            r'^# TOTAL:\s*(\d+)', path.read_text(errors='replace'), re.M))
                    if path.name == 'Makefile' and 'generated by automake' in \
                            path.read_text(errors='replace')[:1000]:
                        automake.append(str(relative))
                    if path.name == 'build.ninja':
                        if '.openmktr' not in path.relative_to(build).parts:
                            raise RuntimeError(f'{label}: generated graph outside .openmktr')
                        ninja_graphs.append(str(relative))
            if not test_totals or sum(test_totals) == 0:
                raise RuntimeError(f'{label}: no nonempty upstream test report retained')
            if mode == 'ninja' and not ninja_graphs:
                raise RuntimeError(f'{label}: compile produced no Ninja graph')
            recursive = len(automake) > 1 and bool(re.search(r'Making (?:all|check) in ', log))
            if package == 'libpipeline' and not recursive:
                raise RuntimeError(f'{label}: recursive Automake execution not demonstrated')
            if package == 'hello':
                run(['/usr/bin/hello'], output, label + '-smoke')
                if (output / (label + '-smoke.log')).read_text().strip() != 'Hello, world!':
                    raise RuntimeError('installed hello produced unexpected output')
            elif package == 'gzip':
                payload = b'Openmktr Portage compression round trip\n' * 20
                compressed = subprocess.check_output(['/bin/gzip-reference', '-c'], input=payload)
                if subprocess.check_output(['/bin/gzip-reference', '-dc'], input=compressed) != payload:
                    raise RuntimeError('installed gzip failed round trip')
                (evidence / 'smoke.txt').write_text('gzip compression/decompression round trip passed\n')
            elif package == 'libpipeline':
                probe = evidence / 'pipeline-smoke.c'
                probe.write_text('#include <stddef.h>\n#include <pipeline.h>\n'
                                 'int main(void) { pipeline *p = pipeline_new_command_args('
                                 '"printf", "Openmktr pipeline\\n", NULL); '
                                 'return pipeline_run(p); }\n')
                run(['clang', probe, '-lpipeline', '-o', evidence / 'pipeline-smoke'],
                    output, label + '-smoke-build')
                run([evidence / 'pipeline-smoke'], output, label + '-smoke')
                if (output / (label + '-smoke.log')).read_text().strip() != 'Openmktr pipeline':
                    raise RuntimeError('installed libpipeline produced unexpected output')
            package_result = {'atom': atom, 'mode': mode, 'status': 'passed',
                              'upstream_tests': sum(test_totals),
                              'recursive_automake': recursive, 'automake_makefiles': automake,
                              'ninja_graphs': ninja_graphs, 'retained_files': retained}
            atomic_json(evidence / 'result.json', package_result)
            result['real_packages'].append(package_result)
            atomic_json(output / 'result.json', result)


def lifecycle(output, result, real_packages):
    cleanup = []
    safe_directory = None
    original_ninja = None
    with tempfile.TemporaryDirectory(prefix='openmktr-portage-', dir='/var/tmp') as temp:
        work = Path(temp)
        work.chmod(0o755)
        try:
            overlay = work / 'overlay'
            shutil.copytree(ROOT / 'packaging/gentoo', overlay)
            source = work / 'source'
            def ignore(directory, names):
                excluded = {'.git', '__pycache__'}
                if Path(directory) == ROOT:
                    excluded.update(('out', 'omktr', 'omktr-sanitized', 'ckati',
                                     'ckati-sanitized', '.ninja_log', '.ninja_deps'))
                return excluded.intersection(names)

            shutil.copytree(ROOT, source, ignore=ignore)
            run(['git', 'init', '-b', 'main'], output, 'source-init', cwd=source)
            run(['git', 'add', '.'], output, 'source-add', cwd=source)
            run(['git', '-c', 'user.name=Openmktr validation', '-c',
                 'user.email=validation@example.invalid', 'commit', '-m', 'Candidate source'],
                output, 'source-commit', cwd=source)
            result['candidate_commit'] = subprocess.check_output(
                ['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
            result['candidate_tree'] = subprocess.check_output(
                ['git', 'rev-parse', 'HEAD^{tree}'], cwd=source, text=True).strip()
            atomic_json(output / 'result.json', result)
            # git-r3 may fetch as root or as portage depending on FEATURES.
            # Trust only this temporary source path for either identity.
            run(['git', 'config', '--system', '--add', 'safe.directory', source],
                output, 'source-ownership')
            safe_directory = source
            # Local fetches may run as Portage's unprivileged user.
            shutil.chown(source, user='portage', group='portage')
            for path in (source / '.git').rglob('*'):
                shutil.chown(path, user='portage', group='portage')
            shutil.chown(source / '.git', user='portage', group='portage')
            for package in PACKAGES:
                pkgdir = overlay / 'dev-build' / package
                pkgdir.mkdir()
                fixture = ROOT / 'validation/compatibility/portage-fixture'
                shutil.copyfile(fixture / 'fixture-1.ebuild', pkgdir / (package + '-1.ebuild'))
                shutil.copytree(fixture / 'files', pkgdir / 'files')
            config = Path('/etc/portage')
            write_new(config / 'repos.conf/openmktr-validation.conf',
                      f'[openmktr]\nlocation = {overlay}\nmasters = gentoo\nauto-sync = no\n', cleanup)
            write_new(config / 'package.accept_keywords/openmktr-validation',
                      '=dev-build/openmktr-9999::openmktr **\n' + ''.join(
                          f'dev-build/{p}::openmktr **\n' for p in PACKAGES), cleanup)
            write_new(config / 'package.use/openmktr-validation',
                      'dev-build/openmktr::openmktr test\n', cleanup)
            write_new(config / 'env/openmktr-validation-source',
                      f'EGIT_OVERRIDE_REPO_RIHARDSPAPS_OPENMKTR="file://{source}"\n'
                      'EGIT_OVERRIDE_BRANCH_RIHARDSPAPS_OPENMKTR="main"\n'
                      'MAKEOPTS="-j4"\nCXXFLAGS="-O1"\n'
                      f'FEATURES="${{FEATURES}} test {NAMESPACE_FEATURES}"\n', cleanup)
            write_new(config / 'package.env/openmktr-validation',
                      'dev-build/openmktr::openmktr openmktr-validation-source\n', cleanup)
            emerge = ['emerge', '--oneshot', '--verbose', '--usepkg=n', '--autounmask=n']
            run(['emerge', '--info'], output, 'environment')
            run([*emerge, '=dev-build/openmktr-9999::openmktr'], output, 'install-openmktr')
            assert Path('/usr/bin/omktr-portage').is_file()
            # Installing the ebuild must not create any activation settings.
            for file in config.rglob('*'):
                if file.is_file() and file not in cleanup:
                    assert 'omktr-portage' not in file.read_text(errors='replace'), file
            run(['python3', str(ROOT / 'validation/compatibility/portage_manager.py')], output, 'installed-manager')
            run(['/usr/bin/omktr', '--version'], output, 'installed-version')
            (output / 'binary.sha256').write_text(
                hashlib.sha256(Path('/usr/bin/omktr').read_bytes()).hexdigest() + '\n')
            # Gentoo may select Samurai globally. The installed Openmktr must
            # still invoke the reference Ninja whose load semantics it needs.
            ninja = Path('/usr/bin/ninja')
            if not ninja.is_symlink() or not Path('/usr/bin/samu').is_file():
                raise RuntimeError('the validation image needs the Ninja alternative and Samurai')
            original_ninja = os.readlink(ninja)
            ninja.unlink()
            ninja.symlink_to('/usr/bin/samu')
            result['global_ninja_alternative'] = 'samurai'
            envfile = config / 'env/openmktr-validation-fixture'
            write_new(envfile, '', cleanup)
            with (config / 'package.env/openmktr-validation').open('a') as stream:
                stream.write('dev-build/openmktr-portage-fixture::openmktr openmktr-validation-fixture\n'
                             'dev-build/openmktr-portage-control::openmktr openmktr-validation-fixture\n')
            for mode, package, fail, fallback in (
                    ('', PACKAGES[1], False, False), ('direct', PACKAGES[0], False, False),
                    ('ninja', PACKAGES[0], False, False), ('direct', PACKAGES[0], False, True),
                    ('ninja', PACKAGES[0], False, True), ('direct', PACKAGES[0], True, False),
                    ('ninja', PACKAGES[0], True, False)):
                label = ((mode or 'control') + ('-failure' if fail else '')
                         + ('-test-fallback' if fallback else ''))
                settings = ('CC="clang"\nCFLAGS="-O1 -DPORTAGE_FIXTURE=7"\n'
                            'MAKEOPTS="-j4 -l1000000"\n'
                            f'FEATURES="${{FEATURES}} test {NAMESPACE_FEATURES}"\n')
                if mode:
                    settings += ('export MAKE="/usr/bin/omktr-portage"\n'
                                 f'export OMKTR_PORTAGE_MODE="{mode}"\n')
                if fail:
                    settings += 'export OMKTR_FIXTURE_FAIL="1"\n'
                if fallback:
                    settings += 'export OMKTR_FIXTURE_TEST_FALLBACK="1"\n'
                envfile.write_text(settings)
                run([*emerge, f'=dev-build/{package}-1::openmktr'], output,
                    label + '-merge', success=not fail)
                installed = Path('/usr/bin') / package
                if fail:
                    assert not installed.exists(), 'Failed compile reached merge'
                    assert 'intentional Portage fixture failure' in (
                        output / (label + '-merge.log')).read_text()
                else:
                    assert subprocess.check_output([str(installed)], text=True).strip() == \
                        'marker=7 version=2'
                    evidence = output / label
                    shutil.copytree(Path('/usr/share') / package, evidence)
                    run(['emerge', '--unmerge', f'dev-build/{package}'], output, label + '-unmerge')
                    assert not installed.exists()
            if real_packages:
                qualify_real_packages(real_packages, output, config, cleanup, result)
            run(['python3', str(ROOT / 'validation/compatibility/portage_workspace.py'),
                 '--output', str(output / 'workspaces')], output, 'retained-workspaces')
            run(['emerge', '--unmerge', 'dev-build/openmktr'], output, 'unmerge-openmktr')
            assert not Path('/usr/bin/omktr-portage').exists()
            assert not Path('/usr/bin/omktr').exists()
            result['coverage'] = 'local EAPI 8 fixtures and listed real packages only'
        finally:
            if original_ninja is not None:
                Path('/usr/bin/ninja').unlink()
                Path('/usr/bin/ninja').symlink_to(original_ninja)
            if safe_directory is not None:
                subprocess.run(['git', 'config', '--system', '--fixed-value',
                                '--unset-all', 'safe.directory', str(safe_directory)],
                               check=True)
            for path in reversed(cleanup):
                path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('/portage-evidence'))
    parser.add_argument('--real-packages', action='store_true',
                        help='also build, test, and merge the pinned real-package set in both modes')
    parser.add_argument('--real-package', action='append', choices=REAL_PACKAGES,
                        help='select a pinned package for a focused real-package rerun (repeatable)')
    args = parser.parse_args()
    if (os.environ.get('OMKTR_PORTAGE_VALIDATION') != '1' or os.geteuid() != 0
            or not Path('/etc/gentoo-release').exists()):
        parser.error('requires a disposable Gentoo container and OMKTR_PORTAGE_VALIDATION=1')
    if any(Path('/usr/bin', name).exists() for name in ('omktr', 'omktr-portage')):
        parser.error('refusing to replace an existing Openmktr installation')
    real_packages = args.real_package or (REAL_PACKAGES if args.real_packages else ())
    with evidence_run(args.output.resolve()) as result:
        lifecycle(args.output.resolve(), result, real_packages)


if __name__ == '__main__':
    main()
