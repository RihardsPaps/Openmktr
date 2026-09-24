#!/usr/bin/env python3
"""Write a reproducible source revision for the build."""

import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
revision = os.environ.get("KATI_SOURCE_REVISION", "")
if not revision:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--short=12", "HEAD"],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    if result.returncode == 0:
        revision = result.stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=normal"],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
        if dirty.returncode == 0 and dirty.stdout:
            revision += "+dirty"
if not revision:
    revision = "unversioned"
content = f"const char* kGitVersion = {json.dumps(revision)};\n"
target = Path(sys.argv[1])
if not target.exists() or target.read_text(encoding="utf-8") != content:
    target.write_text(content, encoding="utf-8")
