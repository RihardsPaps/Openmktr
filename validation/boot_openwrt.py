#!/usr/bin/env python3
"""Boot the pinned OpenWrt x86 image under the campaign-built QEMU."""

import gzip
import os
from pathlib import Path
import select
import shutil
import subprocess
import time


base = Path("/root/universal-tool-campaign-20260927")
image_gz = (base / "builds/openwrt-25.12.5/bin/targets/x86/64/"
            "openwrt-x86-64-generic-ext4-combined.img.gz")
image = base / "builds/openwrt-25.12.5/boot-smoke.img"
qemu = base / "builds/qemu/qemu-system-x86_64"
log = base / "diagnostics/openwrt-boot.log"
log.parent.mkdir(parents=True, exist_ok=True)
prompts = (b"root@OpenWrt", b"root@(none):~#", b"OpenWrt login:")

if not image.exists():
    with gzip.open(image_gz, "rb") as source, image.open("wb") as output:
        shutil.copyfileobj(source, output)

command = [str(qemu), "-m", "1024", "-smp", "2", "-nographic",
           "-monitor", "none", "-no-reboot", "-net", "none",
           "-drive", f"file={image},format=raw,if=ide"]
print("Running", " ".join(command), flush=True)
process = subprocess.Popen(command, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
captured = bytearray()
deadline = time.monotonic() + 100
activated = False
try:
    with log.open("wb") as output:
        while time.monotonic() < deadline:
            readable, _, _ = select.select([process.stdout], [], [], 1)
            if readable:
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                output.write(chunk)
                output.flush()
                captured.extend(chunk)
                if not activated and b"Please press Enter to activate this console." in captured:
                    process.stdin.write(b"\n")
                    process.stdin.flush()
                    activated = True
                if any(prompt in captured for prompt in prompts):
                    print("OpenWrt serial login/shell prompt observed", flush=True)
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

if not any(prompt in captured for prompt in prompts):
    print("OpenWrt prompt not observed; serial log:", log, flush=True)
    raise SystemExit(1)
print("OpenWrt QEMU boot PASS; serial log:", log, flush=True)
