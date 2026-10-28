#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Real curses session check using a private pseudoterminal on Linux."""
import os
from pathlib import Path
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time
import fcntl


def main():
    checks = Path(__file__).with_name('manager.py').resolve()
    with tempfile.TemporaryDirectory(prefix='omktr-curses-') as directory:
        pid, terminal = pty.fork()
        if pid == 0:
            os.environ['TERM'] = 'xterm'
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 120, 0, 0))
            code = ('import importlib.util; from pathlib import Path; '
                    f's=importlib.util.spec_from_file_location("checks", {str(checks)!r}); '
                    'c=importlib.util.module_from_spec(s); s.loader.exec_module(c); '
                    'c.m.save=lambda backend,plan: backend.apply(plan); '
                    f'c.m.tui(c.m.Backend(Path({directory!r}),c.FakePortage()))')
            os.execl(sys.executable, sys.executable, '-c', code)
        output = b''
        def expect(text):
            nonlocal output
            deadline = time.monotonic() + 10
            while text.encode() not in output:
                if time.monotonic() > deadline:
                    raise RuntimeError(f'terminal timed out waiting for {text!r}: {output!r}')
                readable, _, _ = select.select([terminal], [], [], .1)
                if readable:
                    output += os.read(terminal, 65536)
            output = b''
        def send(text): os.write(terminal, text.encode())
        try:
            expect('Openmktr:')
            send('?')
            expect('disabled_graph_optimizations')
            send('\n/')
            expect('Search category/package:')
            send('cat\n')
            send('f')
            expect('Filter: all, installed, available, configured')
            send('installed\n')
            time.sleep(.15)
            fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack('HHHH', 18, 80, 0, 0))
            os.kill(pid, signal.SIGWINCH)
            time.sleep(.15)
            send(' c')
            for name in ('disabled_graph_optimizations', 'build_stats', 'regen_debug', 'mode', 'jobs', 'load', 'graph', 'recursive_cache', 'strict', 'workspace', 'root'):
                expect(name + ':')
                send('\n')
            expect('Staged.')
            send('r')
            expect('BEGIN openmktr-manager')
            send('\ns')
            deadline = time.monotonic() + 10
            while not (Path(directory) / 'package.env').exists():
                if time.monotonic() > deadline: raise RuntimeError('save did not publish configuration')
                time.sleep(.05)
            send('q')
            deadline = time.monotonic() + 10
            while True:
                found, status = os.waitpid(pid, os.WNOHANG)
                if found:
                    pid = None
                    assert os.waitstatus_to_exitcode(status) == 0, status
                    break
                if time.monotonic() > deadline: raise RuntimeError('curses session did not exit')
                time.sleep(.05)
            assert 'cat/pkg openmktr-manager-' in (Path(directory) / 'package.env').read_text()
            print('Real curses select/configure/review/save session passed')
        finally:
            os.close(terminal)
            if pid:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)

if __name__ == '__main__': main()
