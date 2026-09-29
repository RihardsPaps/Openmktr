"""Generate and inspect an EFI image with installed GRUB tools."""
from pathlib import Path
import subprocess
import sys
import tempfile


prefix = Path(sys.argv[1]).resolve()
bin_dir = prefix / "bin"
with tempfile.TemporaryDirectory(prefix="grub-smoke-") as tmp:
    tmp = Path(tmp)
    image = tmp / "grubx64.efi"
    config = tmp / "grub.cfg"
    config.write_text("set timeout=0\nmenuentry 'smoke' { echo ready; }\n")
    subprocess.run([bin_dir / "grub-script-check", config], check=True)
    subprocess.run([bin_dir / "grub-mkimage", "-O", "x86_64-efi", "-o", image,
                    "-p", "/EFI/BOOT", "part_gpt", "fat", "normal", "linux"],
                   check=True)
    kind = subprocess.run(["file", "-b", image], check=True,
                          capture_output=True, text=True).stdout
    assert "PE32+" in kind and "EFI application" in kind, kind
    assert image.stat().st_size > 100_000
    print(f"EFI image {image.stat().st_size} bytes: {kind.strip()}")
