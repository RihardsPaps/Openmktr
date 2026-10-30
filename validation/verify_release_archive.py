#!/usr/bin/env python3
"""Compare original release members without modifying them. MPL-2.0, 2026."""
import hashlib
from pathlib import Path
import sys
import tarfile

archive, source = Path(sys.argv[1]), Path(sys.argv[2])
differences = []
checked = 0
with tarfile.open(archive) as stream:
    for entry in stream:
        relative = Path(*Path(entry.name).parts[1:])
        path = source / relative
        if entry.isfile():
            checked += 1
            expected = hashlib.file_digest(stream.extractfile(entry), 'sha256').digest()
            actual = hashlib.file_digest(path.open('rb'), 'sha256').digest() if path.is_file() else None
            if expected != actual:
                differences.append(str(relative))
        elif entry.issym():
            checked += 1
            if not path.is_symlink() or str(path.readlink()) != entry.linkname:
                differences.append(str(relative))
print(f'{checked} original files/symlinks checked; {len(differences)} differ')
for name in differences:
    print(name)
sys.exit(bool(differences))
