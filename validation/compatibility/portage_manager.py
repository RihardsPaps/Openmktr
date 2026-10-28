#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Installed manager checks, only in the disposable Gentoo qualification lane."""
import json
import os
from pathlib import Path
import subprocess


def main():
    if os.environ.get('OMKTR_PORTAGE_VALIDATION') != '1' or os.geteuid() != 0 or not Path('/etc/gentoo-release').exists():
        raise RuntimeError('requires the disposable Gentoo validation container')
    # Parse every optional frontend dependency, even when installation uses
    # the minimal CLI configuration. Invalid interpreter USE syntax previously
    # masked the entire ebuild before any manager tests could run.
    import portage
    dependencies = portage.db[portage.root]['porttree'].dbapi.aux_get(
        'dev-build/openmktr-9999', ['RDEPEND'])[0]
    for frontend, python_flag in (('gui', 'tk'), ('tui', 'ncurses')):
        atoms = portage.dep.use_reduce(
            dependencies, uselist=[frontend, 'python_single_target_python3_14'],
            flat=True, token_class=portage.dep.Atom)
        assert any(atom.cp == 'dev-lang/python' and python_flag in atom.use.enabled
                   for atom in atoms if atom.use), frontend
    root = Path('/etc/portage')
    snippet = root / 'package.env/openmktr-manager' if (root / 'package.env').is_dir() else root / 'package.env'
    paths = [snippet, root / 'env/openmktr-direct', root / 'bashrc',
             root / '.openmktr-manager.lock', root / '.openmktr-manager-transaction']
    import hashlib
    generated = root / 'env' / ('openmktr-manager-' + hashlib.sha256(b'dev-build/openmktr-portage-fixture').hexdigest()[:24])
    paths.append(generated)
    before = {p: p.read_bytes() if p.exists() else None for p in paths}
    cache = Path('/var/cache/openmktr/workspaces')
    cache_existed = cache.exists()
    def run(*args):
        result = subprocess.run(['/usr/bin/omktr-manager', *args], text=True, capture_output=True)
        if result.returncode: raise RuntimeError(result.stderr)
        print(result.stdout)
        return result.stdout
    try:
        packages = json.loads(run('packages', '--filter', 'installed', '--search', 'sys-apps/portage', '--json'))
        assert any(p['package'] == 'sys-apps/portage' for p in packages)
        run('configure', 'dev-build/openmktr-portage-fixture', '--set', 'mode=ninja', '--dry-run')
        run('configure', 'dev-build/openmktr-portage-fixture', '--set', 'mode=ninja')
        info = json.loads(run('show', 'dev-build/openmktr-portage-fixture', '--json'))[0]
        assert info['state'] == 'managed' and info['settings']['mode'] == 'ninja'
        run('disable', 'dev-build/openmktr-portage-fixture')
        if not before[root / 'bashrc'] and not cache_existed:
            import pwd
            run('setup-workspaces', '--dry-run')
            assert not cache.exists()
            run('setup-workspaces')
            assert cache.stat().st_uid == pwd.getpwnam('portage').pw_uid
            run('configure', 'dev-build/openmktr-portage-fixture', '--set', 'mode=ninja', '--set', 'workspace=true')
            run('disable', 'dev-build/openmktr-portage-fixture')
        env = root / 'env/openmktr-direct'
        if before[env] is None:
            env.write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="direct"\n')
            snippet.write_text(snippet.read_text() + 'dev-build/openmktr-portage-fixture openmktr-direct\n')
            run('adopt', 'dev-build/openmktr-portage-fixture')
            assert json.loads(run('show', 'dev-build/openmktr-portage-fixture', '--json'))[0]['state'] == 'managed'
            run('disable', 'dev-build/openmktr-portage-fixture')
    finally:
        if not cache_existed and cache.exists(): cache.rmdir()
        for path, data in before.items():
            if data is None: path.unlink(missing_ok=True)
            else: path.write_bytes(data)

if __name__ == '__main__': main()
