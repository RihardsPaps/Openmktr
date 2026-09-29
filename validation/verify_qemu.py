"""Verify all configured QEMU emulators and a disk-image round trip."""
from pathlib import Path
import subprocess
import sys
import tempfile


build = Path(sys.argv[1])
targets = next(line.split("=", 1)[1].split() for line in
               (build / "config-host.mak").read_text().splitlines()
               if line.startswith("TARGET_DIRS="))
for target in targets:
    if target.endswith("-softmmu"):
        name = "qemu-system-" + target.removesuffix("-softmmu")
    elif target.endswith("-linux-user"):
        name = "qemu-" + target.removesuffix("-linux-user")
    else:
        raise ValueError(target)
    result = subprocess.run([str(build / name), "--version"],
                            text=True, capture_output=True, check=True)
    assert "9.2.2" in result.stdout, result.stdout
print(f"{len(targets)} configured emulator version checks PASS")
with tempfile.TemporaryDirectory(prefix="qemu-smoke-") as directory:
    raw, qcow, restored = [Path(directory) / name for name in ("input", "disk.qcow2", "output")]
    raw.write_bytes(bytes(range(256)) * 4096)
    image_tool = str(build / "qemu-img")
    subprocess.run([image_tool, "convert", "-f", "raw", "-O", "qcow2", str(raw), str(qcow)], check=True)
    subprocess.run([image_tool, "check", str(qcow)], check=True)
    subprocess.run([image_tool, "convert", "-f", "qcow2", "-O", "raw", str(qcow), str(restored)], check=True)
    assert raw.read_bytes() == restored.read_bytes()
print("qemu-img qcow2 check and byte-exact round trip PASS")
