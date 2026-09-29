#!/usr/bin/env python3
"""Verify the pinned Poky source and deployed minimal-image artifacts."""

import hashlib
from pathlib import Path
import subprocess


base = Path("/root/universal-tool-campaign-20260927")
source = base / "sources/poky-5.0.10"
deploy = base / "builds/yocto-5.0.10/tmp/deploy/images/qemux86-64"


def git(*args):
    return subprocess.check_output(["git", "-C", str(source), *args],
                                   text=True).strip()


commit = git("rev-parse", "HEAD")
expected = "ac257900c33754957b2696529682029d997a8f28"
assert commit == expected, (commit, expected)
assert git("status", "--porcelain") == "", "Poky source checkout changed"
assert git("describe", "--tags", "--exact-match") == "yocto-5.0.10"
print("Poky source tag and clean checkout PASS:", commit)

for pattern in ("core-image-minimal-*.ext4", "bzImage--*.bin",
                "core-image-minimal-*.manifest"):
    matches = list(deploy.glob(pattern))
    assert matches, f"No deployed artifact matching {pattern}"
    item = max(matches, key=lambda path: path.stat().st_mtime)
    assert item.stat().st_size > 0, item
    digest = hashlib.sha256()
    with item.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    print(f"{item.name}: {item.stat().st_size} bytes sha256={digest.hexdigest()}")
