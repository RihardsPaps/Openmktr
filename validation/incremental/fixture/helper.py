#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Deterministic generation and synchronized interruption inside a recipe."""
from pathlib import Path
import subprocess
import sys
import time

if sys.argv[1] == "generate":
    Path(sys.argv[3]).write_bytes(Path(sys.argv[2]).read_bytes())
elif sys.argv[1] == "compile":
    target = Path(sys.argv[2])
    if target.name == "worker.o" and Path(".interrupt-arm").exists():
        # Simulate an interrupted compiler that has already damaged its output.
        target.write_bytes(b"incomplete object\n")
        Path(".interrupt-ready").write_text("partial output written\n")
        time.sleep(60)
    sys.exit(subprocess.call(sys.argv[3:]))
else:
    raise SystemExit("Unknown helper action")
