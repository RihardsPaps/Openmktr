#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Reject missing, changed, or additional reference image packages."""
from pathlib import Path
import subprocess
import sys


def check(path):
    expected = {line.replace("=", "-", 1) for line in path.read_text().splitlines()
                if line and not line.startswith("#")}
    installed = set(subprocess.check_output(["apk", "info", "-v"], text=True).splitlines())
    if installed != expected:
        raise SystemExit("Reference package mismatch: missing=" + str(sorted(expected - installed)) +
                         "; unexpected=" + str(sorted(installed - expected)))
    subprocess.run(["python3", "-c", "import compression.zstd, lzma"], check=True)
    print(f"Verified {len(expected)} locked reference packages and workspace compression modules")


if __name__ == "__main__":
    check(Path(sys.argv[1] if len(sys.argv) > 1 else "tools/chimera-packages.lock"))
