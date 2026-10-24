#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Count live fixture jobs without relying on command output ordering."""
import fcntl
import json
from pathlib import Path
import sys
import time


def update(delta):
    with open('counts', 'r+') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        active, peak, done = json.load(stream)
        active += delta
        stream.seek(0)
        json.dump([active, max(peak, active), done + (delta < 0)], stream)
        stream.truncate()


if sys.argv[1] == 'reset':
    Path('counts').write_text('[0, 0, 0]')
elif sys.argv[1] == 'verify':
    active, peak, done = json.loads(Path('counts').read_text())
    assert active == 0 and done == 4, (active, peak, done)
    assert (peak == 1 if sys.argv[2] == 'serial' else 1 < peak <= 4), peak
else:
    update(1)
    time.sleep(0.25)
    update(-1)
