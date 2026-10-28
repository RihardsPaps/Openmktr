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
    root = Path('/etc/portage')
    snippet = root / 'package.env/openmktr-manager' if (root / 'package.env').is_dir() else root / 'package.env'
    paths = [snippet, root / 'env/openmktr-direct', root / 'bashrc',
             root / '.openmktr-manager.lock', root / '.openmktr-manager-transaction']
    import hashlib
    generated = root / 'env' / ('openmktr-manager-' + hashlib.sha256(b'dev-build/openmktr-portage-fixture').hexdigest()[:24])
    paths.append(generated)
    before = {p: p.read_bytes() if p.exists() else None for p in paths}
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
        env = root / 'env/openmktr-direct'
        if before[env] is None:
            env.write_text('export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="direct"\n')
            snippet.write_text(snippet.read_text() + 'dev-build/openmktr-portage-fixture openmktr-direct\n')
            run('adopt', 'dev-build/openmktr-portage-fixture')
            assert json.loads(run('show', 'dev-build/openmktr-portage-fixture', '--json'))[0]['state'] == 'managed'
            run('disable', 'dev-build/openmktr-portage-fixture')
    finally:
        for path, data in before.items():
            if data is None: path.unlink(missing_ok=True)
            else: path.write_bytes(data)

if __name__ == '__main__': main()
