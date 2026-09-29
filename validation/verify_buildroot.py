#!/usr/bin/env python3
"""Boot the selected Buildroot image and verify its serial login banner."""

import pathlib
import subprocess
import sys


def main() -> int:
    images = pathlib.Path(sys.argv[1])
    for artifact in ("bzImage", "rootfs.ext2", "start-qemu.sh"):
        if not (images / artifact).is_file():
            raise SystemExit(f"missing Buildroot artifact: {artifact}")

    process = subprocess.Popen(
        [str(images / "start-qemu.sh"), "--serial-only", "--", "-m", "512",
         "-no-reboot"],
        cwd=images,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        output, _ = process.communicate(timeout=75)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            output, _ = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
    text = output.decode("utf-8", "replace")
    print(text[-8000:])
    if "buildroot login:" not in text.lower():
        raise SystemExit("Buildroot serial login prompt not observed")
    print("Buildroot kernel and ext2 root filesystem booted to a serial login")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
