"""Prepare the repository's pinned Chimera image as a WSL-native chroot."""
import hashlib
import json
import posixpath
from pathlib import Path
import shutil
import tarfile

base = Path("/root/universal-tool-campaign-20260927")
image = base / "chimera-image"
root = base / "chimera-root-complete"
root.mkdir()


def rootfs_filter(member, destination):
    # Absolute links name paths inside the guest. Rewrite them as relative
    # guest links so Python's extraction filter can verify containment.
    if member.linkname.startswith("/"):
        target = member.linkname.lstrip("/")
        if member.issym():
            target = posixpath.relpath(target, posixpath.dirname(member.name) or ".")
        member = member.replace(linkname=target, deep=False)
    return tarfile.data_filter(member, destination)


manifest = json.loads((image / "manifest.json").read_text())
for layer in manifest["layers"]:
    digest = layer["digest"].split(":", 1)[1]
    archive = image / digest
    with archive.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == digest
    with tarfile.open(archive) as stream:
        stream.extractall(root, filter=rootfs_filter)
shutil.copy2("/etc/resolv.conf", root / "etc/resolv.conf")
print(root)
