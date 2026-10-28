#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Real pkexec allow/deny checks in a disposable Gentoo container.

Requires root, a running private system D-Bus/polkit, an installed manager,
and the omktr-manager-test account. The temporary rule authorizes only this
account to execute this installed helper. It is removed before exit.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import subprocess
import sys
import time


def main():
    if os.environ.get('OMKTR_PORTAGE_VALIDATION') != '1' or os.geteuid() != 0 or not Path('/etc/gentoo-release').exists():
        raise RuntimeError('requires explicit disposable-container validation')
    helper = Path('/usr/bin/omktr-manager')
    sys.path = [p for p in sys.path if Path(p).resolve() != Path(__file__).resolve().parent]
    # Executable helpers have no .py suffix; use an explicit source loader.
    from importlib.machinery import SourceFileLoader
    loader = importlib.util.spec_from_loader('manager', SourceFileLoader('manager', str(helper)))
    manager = importlib.util.module_from_spec(loader)
    loader.loader.exec_module(manager)
    backend = manager.Backend()
    package = 'app-misc/openmktr-manager-validation'
    plan = backend.plan('configure', [package], {})
    root = Path('/etc/portage')
    generated = root / 'env' / ('openmktr-manager-' + hashlib.sha256(package.encode()).hexdigest()[:24])
    touched = [root / p for p in plan['changes']] + [root / '.openmktr-manager.lock']
    before = {p: p.read_bytes() if p.exists() else None for p in touched}
    user = pwd.getpwnam('omktr-manager-test')
    rule = Path('/etc/polkit-1/rules.d/00-openmktr-validation.rules')
    if rule.exists(): raise RuntimeError('test rule already exists')
    code = ("import importlib.util,json,sys; from importlib.machinery import SourceFileLoader; "
            "s=importlib.util.spec_from_loader('manager',SourceFileLoader('manager','/usr/bin/omktr-manager')); "
            "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); "
            "m.save(m.Backend(),json.load(sys.stdin))")
    def demote():
        os.setgroups([])
        os.setgid(user.pw_gid)
        os.setuid(user.pw_uid)
    def invoke():
        return subprocess.run([sys.executable, '-c', code], input=json.dumps(plan),
                              text=True, capture_output=True, preexec_fn=demote, timeout=15,
                              env={**os.environ, 'HOME': user.pw_dir})
    try:
        denied = invoke()
        assert denied.returncode != 0, denied.stdout
        assert not generated.exists(), 'unauthorized save changed configuration'
        print('Real pkexec denial preserves configuration')
        rule.write_text('polkit.addRule(function(action, subject) {\n'
                        ' if (action.id == "org.freedesktop.policykit.exec" &&\n'
                        '     action.lookup("program") == "/usr/bin/omktr-manager" &&\n'
                        '     subject.user == "omktr-manager-test") return polkit.Result.YES;\n'
                        '});\n')
        deadline = time.monotonic() + 10
        while True:
            result = invoke()
            if result.returncode == 0: break
            if time.monotonic() > deadline: raise RuntimeError(result.stderr)
            time.sleep(.2)
        assert generated.stat().st_uid == 0
        assert backend.show(package)['state'] == 'managed'
        print('Real pkexec authorization elevates only the helper and saves configuration')
        stale = invoke()
        assert stale.returncode != 0
        assert backend.show(package)['state'] == 'managed'
        print('Privileged helper rejects stale request after elevation')
    finally:
        rule.unlink(missing_ok=True)
        for path, data in before.items():
            if data is None: path.unlink(missing_ok=True)
            else: path.write_bytes(data)

if __name__ == '__main__': main()
