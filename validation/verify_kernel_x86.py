#!/usr/bin/env python3
"""Boot the ckati-built Linux kernel with the previously built root image."""

from pathlib import Path
import select
import subprocess
import sys
import time


qemu, kernel, rootfs = map(lambda item: Path(item).resolve(), sys.argv[1:4])
command = [
    str(qemu), "-nographic", "-no-reboot", "-monitor", "none",
    "-serial", "stdio", "-m", "1024", "-kernel", str(kernel),
    "-drive", f"file={rootfs},if=virtio,format=raw,readonly=on",
    "-append", "root=/dev/vda console=ttyS0 rootwait",
]
process = subprocess.Popen(command, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
output = bytearray()
deadline = time.monotonic() + 60
try:
    while time.monotonic() < deadline:
        readable, _, _ = select.select([process.stdout], [], [], 1)
        if readable:
            chunk = process.stdout.read1(65536)
            if not chunk:
                break
            output.extend(chunk)
            if b"buildroot login:" in output or b"Welcome to Buildroot" in output:
                break
        if process.poll() is not None:
            break
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()

transcript = output.decode("utf-8", "replace")
print(transcript[-6000:])
if "Linux version 6.12.0" not in transcript:
    raise SystemExit("kernel version banner not seen")
if "buildroot login:" not in transcript and "Welcome to Buildroot" not in transcript:
    raise SystemExit("Buildroot userspace did not start")
