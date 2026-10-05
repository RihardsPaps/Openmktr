#!/usr/bin/env python3
"""Configure every Linux architecture with omktr in separate output trees."""

from pathlib import Path
import subprocess
import sys


campaign = Path(sys.argv[1]).resolve()
report = Path(sys.argv[2]).resolve()
source = campaign / "sources/linux-6.12"
architectures = sorted(
    directory.name for directory in (source / "arch").iterdir()
    if directory.is_dir() and (directory / "Kconfig").exists()
)
failed = []
for architecture in architectures:
    output = campaign / "builds" / f"linux-6.12-{architecture}"
    result = subprocess.run(
        [sys.executable, str(report / "phase.py"),
         "--report", str(report),
         "--name", f"wsl-linux-6.12-{architecture}-defconfig",
         "--cwd", str(source), "--", str(campaign / "tool/omktr"),
         f"O={output}", f"ARCH={architecture}", "defconfig"],
        check=False,
    )
    if result.returncode:
        failed.append(architecture)
print(f"Linux 6.12 defconfig: {len(architectures) - len(failed)}/"
      f"{len(architectures)} architecture directories passed")
if failed:
    print("Failed:", ", ".join(failed))
raise SystemExit(bool(failed))
