"""Boot the pinned TF-A, OP-TEE and U-Boot images together in QEMU."""
from pathlib import Path
import subprocess
import sys
import tempfile


qemu, bios = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="firmware-stack-") as temp:
    normal = Path(temp) / "normal.log"
    secure = Path(temp) / "secure.log"
    command = [str(qemu), "-machine", "virt,secure=on", "-cpu", "cortex-a57",
               "-m", "1024", "-smp", "2", "-display", "none",
               "-serial", f"file:{normal}", "-serial", f"file:{secure}",
               "-monitor", "none", "-bios", str(bios), "-no-reboot"]
    try:
        subprocess.run(command, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=8, check=False)
    except subprocess.TimeoutExpired:
        pass
    normal_log = normal.read_text(errors="replace")
    secure_log = secure.read_text(errors="replace")
    for marker in ("BL1: Booting BL2", "BL1: Booting BL31",
                   "U-Boot 2025.01", "=>"):
        if marker not in normal_log:
            raise SystemExit(f"missing normal-world boot marker: {marker}")
    for marker in ("OP-TEE version:", "Primary CPU switching to normal world"):
        if marker not in secure_log:
            raise SystemExit(f"missing secure-world boot marker: {marker}")
    print("TF-A BL1/BL2/BL31, OP-TEE and U-Boot booted in QEMU")
