#!/usr/bin/env python3
"""Check changed C++ files with the campaign's Chimera clang-format."""

from pathlib import Path
import subprocess
import sys


root = Path("/mnt/c/Users/Harry/Documents/ChatGPT/Universal Tool")
chroot = "/root/universal-tool-campaign-20260927/chimera-root-complete"
changed = subprocess.check_output(
    ["git", "-c", "core.autocrlf=true", "-C", str(root),
     "diff", "--name-only", "--", "src"],
    text=True,
).splitlines()
unformatted = []
for name in changed:
    if not name.endswith((".cc", ".h")):
        continue
    path = root / name
    original = path.read_bytes().replace(b"\r\n", b"\n")
    formatted = subprocess.check_output(
        ["chroot", chroot, "clang-format", "--assume-filename=/workspace/" + name],
        input=original,
    )
    if original != formatted:
        unformatted.append(name)
        if "--fix" in sys.argv:
            path.write_bytes(formatted.replace(b"\n", b"\r\n"))
for name in unformatted:
    print("Formatted" if "--fix" in sys.argv else "Formatting differs:", name)
raise SystemExit(bool(unformatted) and "--fix" not in sys.argv)
