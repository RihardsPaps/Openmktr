#!/usr/bin/env python3
"""Boot the pinned Poky minimal image in the campaign-built QEMU."""

import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import time


base = Path("/root/universal-tool-campaign-20260927")
deploy = base / "builds/yocto-5.0.10/tmp/deploy/images/qemux86-64"
kernel = max(deploy.glob("bzImage--*.bin"), key=lambda path: path.stat().st_mtime)
rootfs = max(deploy.glob("core-image-minimal-*.ext4"),
             key=lambda path: path.stat().st_mtime)
disk = base / "diagnostics/yocto-boot.img"
log = base / "diagnostics/yocto-boot.log"
disk.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(rootfs, disk)
command = [str(base / "builds/qemu/qemu-system-x86_64"),
           "-m", "1024", "-smp", "2", "-cpu", "IvyBridge", "-machine", "q35",
           "-nographic", "-monitor", "none",
           "-no-reboot", "-net", "none", "-kernel", str(kernel),
           "-append", "root=/dev/sda rw console=ttyS0",
           "-drive", f"file={disk},format=raw,if=ide"]
print("Running", " ".join(command), flush=True)
process = subprocess.Popen(command, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
captured = bytearray()
deadline = time.monotonic() + 120
sent_login = False
sent_probe = False
marker = b"CODEX_YOCTO_BOOT_OK"
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
                if not sent_login and b"login:" in captured:
                    process.stdin.write(b"root\n")
                    process.stdin.flush()
                    sent_login = True
                if sent_login and not sent_probe and re.search(
                        rb"root@[^\r\n]+[:#]", captured):
                    process.stdin.write(b"echo " + marker + b"\n")
                    process.stdin.flush()
                    sent_probe = True
                if sent_probe and captured.count(marker) >= 2:
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

if captured.count(marker) < 2:
    print("Poky shell probe not observed; serial log:", log, flush=True)
    raise SystemExit(1)
print("Poky QEMU boot and root-shell probe PASS; serial log:", log, flush=True)
