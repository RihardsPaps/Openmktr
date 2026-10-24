# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Controlled C/C++ compilation inputs for the optional ccache campaign."""

import os
from pathlib import Path
import re
import signal
import shutil
import subprocess

from cross_version_cache import PORTAGE, counter_delta, file_inventory, require_equal, restore_seed


CASES = ('relocation', 'revision', 'source', 'patch', 'use', 'options', 'compiler',
         'generated', 'transitive', 'system', 'preserved-mtime', 'new-header',
         'search-order', 'abi', 'dependency-version', 'static-library', 'dynamic-library', 'debug-file',
         'time-macros', 'failure', 'missing-entry', 'corrupt-entry', 'eviction',
         'concurrent', 'incremental')


def write_tree(root, campaign, case):
    root.mkdir(parents=True)
    (root / 'child').mkdir()
    for directory in ('first', 'second', 'sys'):
        (root / directory).mkdir()
    (root / 'config.txt').write_text('3\n')
    (root / 'generate.py').write_text(
        'from pathlib import Path\n'
        'Path("generated.h").write_text("#define GENERATED " + Path("config.txt").read_text())\n')
    (root / 'nested.h').write_text('#define NESTED 4\n')
    (root / 'transitive.h').write_text('#include "nested.h"\n')
    (root / 'sys/system.h').write_text('#define SYSTEM 5\n')
    (root / 'second/choice.h').write_text('#define CHOICE 6\n')
    (root / 'dependency.h').write_text('struct dependency { int x; };\n')
    (root / 'library.c').write_text('int library_value(void) { return 7; }\n')
    (root / 'child/unit.cc').write_text('extern "C" int unit_value(void) { return 8; }\n')
    source = ('#include <stdio.h>\n#include "generated.h"\n#include "transitive.h"\n'
              '#include <system.h>\n#include <choice.h>\n#include "dependency.h"\n'
              '#if __has_include("optional.h")\n#include "optional.h"\n'
              '#else\n#define OPTIONAL 0\n#endif\n'
              '#ifndef FEATURE\n#define FEATURE 0\n#endif\n'
              'int library_value(void); int unit_value(void);\n'
              'int main(void) { printf("%d\\n", GENERATED + NESTED + SYSTEM + CHOICE + '
              'OPTIONAL + FEATURE + library_value() + unit_value() + '
              '(int)sizeof(struct dependency));\n')
    if case == 'debug-file':
        source += 'puts(__FILE__);\n'
    if case == 'time-macros':
        source += 'puts(__DATE__ " " __TIME__);\n'
    (root / 'main.c').write_text(source + 'return 0; }\n')
    flags = '-O2 -g' if case == 'debug-file' else '-O2'
    common = (f'CC = {campaign.work}/cc\nCXX = {campaign.work}/cxx\n'
              f'CFLAGS = {flags}\nCXXFLAGS = $(CFLAGS) -stdlib=libc++\nMAKE = {campaign.binary}\n')
    (root / 'config.mk').write_text(common)
    shared = case == 'dynamic-library'
    library = 'libvalue.so' if shared else 'libvalue.a'
    link = "-L. -lvalue -Wl,-rpath,'$$ORIGIN'" if shared else 'libvalue.a'
    lib_recipe = ('$(CC) -shared library.o -o libvalue.so' if shared else
                  'llvm-ar rcs libvalue.a library.o')
    (root / 'Makefile').write_text(
        'include config.mk\n.PHONY: all child\nall: app\n'
        'generated.h: config.txt generate.py\n\tpython3 generate.py\n'
        'main.o: main.c generated.h\n'
        '\t$(CC) $(CFLAGS) -I. -Ifirst -Isecond -isystem sys -MD -MF main.d -c main.c -o main.o\n'
        'library.o: library.c\n\t$(CC) $(CFLAGS) -fPIC -MMD -MF library.d -c library.c -o library.o\n'
        f'{library}: library.o\n\t{lib_recipe}\n'
        'child:\n\t+$(MAKE) -C child -j2 all\n'
        f'app: main.o {library} child\n\t$(CXX) -stdlib=libc++ main.o child/unit.o {link} -o app\n'
        '-include main.d library.d\n')
    if case == 'debug-file':
        makefile = root / 'Makefile'
        makefile.write_text(makefile.read_text().replace('-c main.c', f'-c {root}/main.c'))
    (root / 'child/Makefile').write_text(
        'include ../config.mk\nall: unit.o\nunit.o: unit.cc\n'
        '\t$(CXX) $(CXXFLAGS) -MMD -MF unit.d -c unit.cc -o unit.o\n-include unit.d\n')


def mutate(root, case):
    changes = {'source': ('main.c', 'return 0;', 'puts("new source"); return 0;'),
               'use': ('config.mk', 'CFLAGS = ', 'CFLAGS = -DFEATURE=10 '),
               'options': ('config.mk', '-O2', '-O0'),
               'generated': ('config.txt', '3', '30'),
               'transitive': ('nested.h', '4', '40'),
               'system': ('sys/system.h', '5', '50'),
               'preserved-mtime': ('nested.h', '4', '41'),
               'abi': ('dependency.h', 'int x;', 'long x; long y;'),
               'dependency-version': ('dependency.h', 'int x;', 'long x[3];'),
               'static-library': ('library.c', '7', '70'),
               'dynamic-library': ('library.c', '7', '71'),
               'failure': ('main.c', 'return 0;', 'invalid syntax! return 0;')}
    if case in changes:
        filename, old, new = changes[case]
        path = root / filename
        st = path.stat()
        path.write_text(path.read_text().replace(old, new))
        if case == 'preserved-mtime':
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
    if case == 'new-header':
        (root / 'optional.h').write_text('#define OPTIONAL 100\n')
    if case == 'search-order':
        (root / 'first/choice.h').write_text('#define CHOICE 60\n')
    if case == 'patch':
        # A real src_prepare patch is exercised separately through an EAPI 8
        # fixture ebuild; this local edit is its resulting compiler input.
        (root / 'library.c').write_text('int library_value(void) { return 72; }\n')


def patch_ebuild(campaign):
    """Isolate a revision whose only semantic difference is src_prepare's patch."""
    overlay = campaign.work / 'patch-overlay'
    package = overlay / 'dev-build/openmktr-reuse-fixture'
    files = package / 'files'
    files.mkdir(parents=True)
    (overlay / 'profiles').mkdir()
    (overlay / 'profiles/repo_name').write_text('openmktr-reuse\n')
    (overlay / 'metadata').mkdir()
    (overlay / 'metadata/layout.conf').write_text('masters = gentoo\nthin-manifests = true\n')
    (files / 'main.c').write_text('#include <stdio.h>\nint main(void) { puts("1"); return 0; }\n')
    (files / 'value.patch').write_text(
        '--- a/main.c\n+++ b/main.c\n@@ -1,2 +1,2 @@\n #include <stdio.h>\n'
        '-int main(void) { puts("1"); return 0; }\n'
        '+int main(void) { puts("2"); return 0; }\n')
    (files / 'Makefile').write_text(
        'all: app\nmain.o: main.c\n\t$(CC) $(CFLAGS) -MD -MF main.d -c main.c -o main.o\n'
        'app: main.o\n\t$(CC) main.o -o app\n-include main.d\n')
    ebuild = ('EAPI=8\nDESCRIPTION="Controlled cross-version patch fixture"\n'
              'HOMEPAGE="https://example.invalid"\nLICENSE="MPL-2.0"\nSLOT="0"\n'
              'KEYWORDS="~amd64"\nS="${WORKDIR}/source"\n'
              'src_unpack() { mkdir -p "${S}"; cp "${FILESDIR}/main.c" "${FILESDIR}/Makefile" "${S}/" || die; }\n'
              'src_compile() { emake; }\n'
              'src_test() { ./app || die; }\n'
              'src_install() { exeinto /usr/bin; doexe app; }\n')
    for version in ('1', '1-r1'):
        (package / f'openmktr-reuse-fixture-{version}.ebuild').write_text(
            ebuild + ('PATCHES=( "${FILESDIR}/value.patch" )\n' if version == '1-r1' else ''))
    bindir = campaign.work / 'patch-bin'
    bindir.mkdir()
    shutil.copyfile(campaign.binary, bindir / 'omktr')
    shutil.copyfile(Path(__file__).resolve().parents[2] / 'packaging/gentoo/omktr-portage',
                    bindir / 'omktr-portage')
    for path in bindir.iterdir():
        path.chmod(0o755)
    cleanup = []
    config = Path('/etc/portage')
    try:
        PORTAGE.write_new(config / 'repos.conf/openmktr-reuse-fixture.conf',
                          f'[openmktr-reuse]\nlocation = {overlay}\nmasters = gentoo\nauto-sync = no\n', cleanup)
        PORTAGE.write_new(config / 'package.env/openmktr-reuse-fixture',
                          'dev-build/openmktr-reuse-fixture openmktr-reuse-fixture\n', cleanup)
        envfile = config / 'env/openmktr-reuse-fixture'
        PORTAGE.write_new(envfile, '', cleanup)
        for mode in campaign.args.mode:
            seed = campaign.work / ('patch-seed-' + mode)
            comparisons = []
            for version, state in (('1', 'cold'), ('1-r1', 'disabled'), ('1-r1', 'upgrade')):
                if state == 'upgrade':
                    restore_seed(seed, campaign.cache)
                elif campaign.cache.exists():
                    shutil.rmtree(campaign.cache)
                label = f'ebuild-patch-{mode}-{version}-{state}'
                work = Path('/var/tmp/portage/dev-build') / ('openmktr-reuse-fixture-' + version) / 'work'
                campaign.select(state, 'original')
                settings = (f'export MAKE="{bindir}/omktr-portage"\n'
                            f'export OMKTR_PORTAGE_MODE="{mode}"\n'
                            f'CC="{campaign.work}/cc"\nCFLAGS="-O2"\nMAKEOPTS="-j4"\n'
                            f'FEATURES="${{FEATURES}} test keepwork keeptemp -ccache {PORTAGE.NAMESPACE_FEATURES}"\n')
                envfile.write_text(settings + 'FEATURES="${FEATURES} -keepwork -keeptemp"\n')
                path = package / f'openmktr-reuse-fixture-{version}.ebuild'
                campaign.run(['ebuild', path, 'clean'], label + '-clean')
                envfile.write_text(settings)
                campaign.run(['ebuild', path, 'unpack', 'prepare', 'configure', 'compile', 'test', 'install'], label)
                runtime = subprocess.check_output([work / 'source/app'], text=True).strip()
                if runtime != ('1' if version == '1' else '2'):
                    raise RuntimeError(label + ': Portage patch did not affect the executable')
                inventory = file_inventory(work)
                if state == 'cold':
                    shutil.copytree(campaign.cache, seed)
                else:
                    comparisons.append(inventory)
            require_equal(comparisons[0]['objects'], comparisons[1]['objects'], label)
            require_equal(comparisons[0]['artifacts'], comparisons[1]['artifacts'], label)
            campaign.result['fixtures'].append({'case': 'ebuild-patch', 'mode': mode,
                                               'profile': 'original', 'status': 'passed'})
            PORTAGE.atomic_json(campaign.output / 'result.json', campaign.result)
    finally:
        for path in reversed(cleanup):
            path.unlink()


def prepare_cache(campaign, seed, case):
    restore_seed(seed, campaign.cache)
    # ccache 4.14's file storage backend uses bare digest names, while older
    # layouts append R to result entries. Never corrupt statistics/config files.
    entries = [p for p in campaign.cache.rglob('*') if p.is_file() and
               re.fullmatch(r'[0-9a-f]{20,}R?', p.name)]
    if case in ('missing-entry', 'corrupt-entry') and not entries:
        raise RuntimeError('fixture did not populate ccache result entries')
    if case == 'missing-entry':
        for path in entries:
            path.unlink()
    if case == 'corrupt-entry':
        for path in entries:
            data = bytearray(path.read_bytes())
            data[-1] ^= 1
            path.write_bytes(data)
    if case == 'eviction':
        campaign.run([campaign.ccache, '--cleanup'], 'fixture-evict',
                     env=campaign.env | {'CCACHE_CONFIGPATH': str(campaign.config),
                                         'CCACHE_MAXSIZE': '1K'})


def build(campaign, root, mode, label, failure=False):
    command = [campaign.binary]
    if mode == 'ninja':
        command.append('build')
    command.extend(['-j' + str(campaign.args.jobs), 'all'])
    measured = campaign.run(command, label, cwd=root, success=not failure)
    inventory = file_inventory(root)
    runtime = None if failure else subprocess.check_output([root / 'app'], text=True)
    records = campaign.retain_journal(label)
    diagnostics = sorted(r['stderr'] for r in records if r['compile'] and r['stderr'])
    return {'inventory': inventory, 'runtime': runtime, 'diagnostics': diagnostics,
            'measurement': measured, 'compiler_calls': len(records)}


def fixtures(campaign):
    result, output = campaign.result, campaign.output
    result['fixtures'] = []
    original_cc = campaign.cc
    alternate_cc = Path(original_cc).parent / 'clang-reuse-identity'
    shutil.copyfile(original_cc, alternate_cc)
    with alternate_cc.open('ab') as stream:
        stream.write(b'\0Openmktr compiler-identity fixture\0')
    alternate_cc.chmod(0o755)
    for mode in campaign.args.mode:
        for profile in campaign.args.profile:
            for case in campaign.args.fixture or CASES:
                if case not in CASES:
                    raise ValueError('unknown fixture: ' + case)
                label = f'fixture-{mode}-{profile}-{case}'
                base = campaign.work / label
                old = base / 'pkg-1.0/work'
                new = base / ('pkg-1.0-r1/work' if case == 'revision' else 'pkg-1.1/work')
                write_tree(old, campaign, case)
                if campaign.cache.exists():
                    shutil.rmtree(campaign.cache)
                campaign.select('cold', profile, [(str(old), '/openmktr-src')])
                campaign.env['SOURCE_DATE_EPOCH'] = '1700000000'
                build(campaign, old, mode, label + '-seed')
                seed = base / 'seed'
                shutil.copytree(campaign.cache, seed)
                write_tree(new, campaign, case)
                mutate(new, case)
                if case == 'time-macros':
                    campaign.env['SOURCE_DATE_EPOCH'] = '1700000100'
                if case == 'compiler':
                    campaign.cc = str(alternate_cc)
                campaign.select('disabled', profile, [(str(new), '/openmktr-src')])
                expected = build(campaign, new, mode, label + '-disabled', case == 'failure')
                # Preserve the exact source state; remove all compiled outputs,
                # graphs and generated files before testing cache restoration.
                shutil.rmtree(new)
                write_tree(new, campaign, case)
                mutate(new, case)
                prepare_cache(campaign, seed, case)
                campaign.select('upgrade', profile, [(str(new), '/openmktr-src')])
                before = campaign.stats()
                actual = build(campaign, new, mode, label + '-upgrade', case == 'failure')
                delta = counter_delta(before, campaign.stats())
                if case in ('relocation', 'revision', 'incremental') and not delta.get('preprocessed_cache_hit'):
                    raise RuntimeError(label + ': expected unchanged compilations to reuse cached results')
                for kind in ('objects', 'artifacts', 'dependencies'):
                    require_equal(expected['inventory'][kind], actual['inventory'][kind], label + kind)
                if expected['runtime'] != actual['runtime'] or expected['diagnostics'] != actual['diagnostics']:
                    raise RuntimeError(label + ': runtime or diagnostics mismatch')
                if case in ('incremental', 'system'):
                    header = new / ('sys/system.h' if case == 'system' else 'nested.h')
                    header.write_text(header.read_text().replace('5', '55').replace('4', '44'))
                    campaign.select('upgrade', profile, [(str(new), '/openmktr-src')])
                    incremental = build(campaign, new, mode, label + '-incremental')
                    # Clean oracle for the same header change catches missing
                    # dependencies even when the cache itself would be correct.
                    for p in new.rglob('*.o'):
                        p.unlink()
                    campaign.select('disabled', profile, [(str(new), '/openmktr-src')])
                    clean = build(campaign, new, mode, label + '-incremental-oracle')
                    require_equal(clean['inventory']['objects'], incremental['inventory']['objects'], label)
                    if clean['runtime'] != incremental['runtime']:
                        raise RuntimeError(label + ': stale incremental output')
                if case == 'concurrent':
                    # Two independent trees publish the same compilations into
                    # one cache concurrently, with separate output destinations.
                    twin = base / 'twin/work'
                    write_tree(twin, campaign, case)
                    campaign.select('upgrade', profile, [(str(new), '/openmktr-src'),
                                                         (str(twin), '/openmktr-src')])
                    for p in new.rglob('*.o'):
                        p.unlink()
                    commands = [[campaign.binary, *(['build'] if mode == 'ninja' else []),
                                 '-j2', 'all'] for _ in range(2)]
                    processes = [subprocess.Popen(command, cwd=root, env=campaign.env,
                                                  start_new_session=True,
                                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                                 for command, root in zip(commands, (new, twin))]
                    try:
                        for index, process in enumerate(processes):
                            log, _ = process.communicate(timeout=60)
                            (output / (label + f'-concurrent-{index}.log')).write_bytes(log)
                            if process.returncode:
                                raise RuntimeError(label + ': concurrent build failed')
                    finally:
                        for process in processes:
                            if process.poll() is None:
                                os.killpg(process.pid, signal.SIGKILL)
                                process.wait()
                    if subprocess.check_output([twin / 'app'], text=True) != expected['runtime']:
                        raise RuntimeError(label + ': concurrent output mismatch')
                    for tree in (new, twin):
                        concurrent = file_inventory(tree)
                        for kind in ('objects', 'artifacts', 'dependencies'):
                            require_equal(expected['inventory'][kind], concurrent[kind],
                                          label + ' concurrent ' + kind)
                campaign.cc = original_cc
                row = {'case': case, 'mode': mode, 'profile': profile,
                       'status': 'passed', 'cache_stats': delta,
                       'runtime': actual['runtime'], 'inventory': actual['inventory']}
                result['fixtures'].append(row)
                PORTAGE.atomic_json(output / 'result.json', result)
                # The detailed compiler journals and output hashes are retained;
                # fixture source trees/caches need not consume campaign disk.
                shutil.rmtree(base)
    alternate_cc.unlink()
    if not campaign.args.fixture or 'patch' in campaign.args.fixture:
        patch_ebuild(campaign)
