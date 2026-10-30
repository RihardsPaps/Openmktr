# Full LibreOffice comparison evidence

The original ZIP exceeds GitHub's 100 MiB file limit. Its complete bytes are
stored in two ordered parts:

- [Part 1](libreoffice-full-comparison-2026-10-29.zip.part01)
- [Part 2](libreoffice-full-comparison-2026-10-29.zip.part02)
- [Original ZIP SHA-256](libreoffice-full-comparison-2026-10-29.zip.sha256)

Run this Python code from the repository root to reconstruct and verify the ZIP.
The reconstructed file is ignored by Git; the evidence contents are unchanged.

```python
from pathlib import Path
import hashlib
import shutil

archive = Path("validation/evidence/libreoffice-full-comparison-2026-10-29.zip")
with archive.open("wb") as output:
    for suffix in (".part01", ".part02"):
        with Path(str(archive) + suffix).open("rb") as part:
            shutil.copyfileobj(part, output)
with archive.open("rb") as source:
    actual = hashlib.file_digest(source, "sha256").hexdigest()
expected = Path(str(archive) + ".sha256").read_text().split()[0]
assert actual == expected, (actual, expected)
print(archive, actual)
```
