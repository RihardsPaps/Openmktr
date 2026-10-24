#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Compiler instrumentation used only by the disposable reuse campaign."""

import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import time
import uuid


def main():
    control = json.loads(Path(sys.argv[1]).read_text())
    compiler = control['cxx'] if sys.argv[2] == 'cxx' else control['cc']
    args = sys.argv[3:]
    compile_only = '-c' in args
    unsupported = any(a.endswith(('.s', '.S', '.pch', '.gch', '.pcm')) or
                      a in ('c-header', 'c++-header', '-emit-pch', '-load') or
                      a.startswith(('-fmodule', '-fplugin', '-fprofile', '-fauto-profile', '-include-pch'))
                      for a in args)
    if control['profile'] == 'normalized':
        # Actual compiler transformations, also applied to the uncached control.
        # No ccache-only path rewriting or dependency-file rewriting is used.
        for root, replacement in control.get('maps', []):
            args.append(f'-ffile-prefix-map={root}={replacement}')
        args.append('-fdebug-compilation-dir=.')
    command = [compiler, *args]
    cached = compile_only and not unsupported and control['cache_state'] != 'disabled'
    env = {k: v for k, v in os.environ.items() if not k.startswith('CCACHE_')}
    if cached:
        env['CCACHE_CONFIGPATH'] = control['config']
        env.update(CCACHE_SLOPPINESS='', CCACHE_BASEDIR='', CCACHE_REMOTE_STORAGE='')
        command.insert(0, control['ccache'])
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    elapsed = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    record = {'command': command, 'cwd': os.getcwd(), 'phase': os.environ.get('EBUILD_PHASE'),
              'compile': compile_only,
              'preprocess': '-E' in args, 'unsupported': unsupported, 'cached': cached,
              'exit_status': result.returncode, 'seconds': elapsed,
              'cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
              'stdout': result.stdout.decode(errors='replace'),
              'stderr': result.stderr.decode(errors='replace')}
    if '-o' in args:
        output = Path(args[args.index('-o') + 1])
        record['output'] = str(output.absolute())
        if output.is_file():
            record['sha256'] = hashlib.sha256(output.read_bytes()).hexdigest()
    journal = Path(control['journal'])
    journal.mkdir(parents=True, exist_ok=True)
    (journal / (uuid.uuid4().hex + '.json')).write_text(json.dumps(record))
    return result.returncode if result.returncode >= 0 else 128 - result.returncode


if __name__ == '__main__':
    sys.exit(main())
