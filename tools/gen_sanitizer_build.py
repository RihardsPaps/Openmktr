#!/usr/bin/env python3
"""Derive an isolated ASan/UBSan build from the normal Ninja graph."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
source = (root / "build.ninja").read_text(encoding="utf-8")
assert "-O2" in source and "-flto=thin" in source
source = source.replace(
    "-O2", "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
)
source = source.replace("-flto=thin", "")
source = source.replace("-Wl,--icf=safe", "")
source = source.replace(
    "ldflags = ", "ldflags = -fsanitize=address,undefined ", 1
)
source = source.replace("out/", "out/sanitized/")
source = source.replace("build out: mkdir", "build out/sanitized: mkdir")
source = source.replace("|| out\n", "|| out/sanitized\n")
source = source.replace("build omktr: link", "build omktr-sanitized: link")
source = source.replace("default omktr\n", "default omktr-sanitized\n")
(root / "build.sanitizer.ninja").write_text(source, encoding="utf-8")
