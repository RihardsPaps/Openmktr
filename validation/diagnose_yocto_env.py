#!/usr/bin/env python3
"""Inspect the environment of a generated BitBake task without running it."""

from pathlib import Path
import sys


original = Path(sys.argv[1]).read_text()
needle = "\ndo_compile\n\n# cleanup"
assert original.count(needle) == 1
replacement = """
env | wc -c
env | awk '{ if (length($0) > max) { max = length($0); name = $0 } }
           END { print max; print substr(name, 1, 80) }'
exit 0

# cleanup"""
result = Path("/tmp/yocto-binutils-env-size.sh")
result.write_text(original.replace(needle, replacement))
result.chmod(0o755)
