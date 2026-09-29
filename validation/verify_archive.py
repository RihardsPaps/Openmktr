"""Check original upstream file contents and symlinks against a pinned archive."""
import hashlib
import os
from pathlib import Path
import sys
import tarfile


archive, source = Path(sys.argv[1]), Path(sys.argv[2])
checked = 0
failures = []
with tarfile.open(archive) as package:
    for member in package:
        parts = Path(member.name).parts
        if len(parts) < 2:
            continue
        target = source.joinpath(*parts[1:])
        if member.isfile():
            checked += 1
            try:
                with package.extractfile(member) as original, target.open("rb") as actual:
                    if hashlib.file_digest(original, "sha256").digest() != hashlib.file_digest(actual, "sha256").digest():
                        failures.append(member.name)
            except OSError:
                failures.append(member.name)
        elif member.issym():
            checked += 1
            if not target.is_symlink() or os.readlink(target) != member.linkname:
                failures.append(member.name)
print(f"{checked} original files/symlinks checked; {len(failures)} differ")
for failure in failures:
    print(failure)
raise SystemExit(bool(failures))
