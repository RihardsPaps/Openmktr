#!/usr/bin/env python3
"""Compare the compiler arguments emitted for one Kbuild object in two logs."""

import difflib
from pathlib import Path
import shlex
import sys


def command(path: str) -> list[str]:
    lines = Path(path).read_text(errors="replace").splitlines()
    matches = [line.strip() for line in lines
               if line.lstrip().startswith("aarch64-linux-gnu-gcc ")
               and "init/main.c" in line]
    if not matches:
        raise ValueError(f"compiler command absent in {path}")
    return shlex.split(matches[0])


left, right = map(command, sys.argv[1:3])
print("".join(difflib.unified_diff(left, right, fromfile="full-build",
                                   tofile="single-object", lineterm="\n")))
