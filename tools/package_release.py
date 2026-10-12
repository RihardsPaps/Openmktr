#!/usr/bin/env python3
"""Package a tested Chimera binary and its corresponding Git source archive.

Copyright 2026 Rihards Paps and Haralds Paps
SPDX-License-Identifier: MPL-2.0
"""

import argparse
import hashlib
import io
from pathlib import Path
import platform
import re
import struct
import subprocess
import tarfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
LICENSE_FILES = (
    'LICENSE', 'NOTICE', 'LICENSES/Apache-2.0.txt',
    'LICENSES/upstream-AUTHORS.txt', 'LICENSES/upstream-CONTRIBUTORS.txt',
    'docs/LICENSING.md',
)


def musl_interpreter(data):
    """Read PT_INTERP from an ELF64 little-endian x86_64 executable."""
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 62:
        raise ValueError('Expected an ELF64 x86_64 executable')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size < 56:
        raise ValueError('Invalid ELF program header size')
    for index in range(count):
        header = offset + index * size
        if struct.unpack_from('<I', data, header)[0] == 3:
            start = struct.unpack_from('<Q', data, header + 8)[0]
            length = struct.unpack_from('<Q', data, header + 32)[0]
            interpreter = data[start:start + length]
            if interpreter != b'/lib/ld-musl-x86_64.so.1\0':
                raise ValueError('Binary must use the x86_64 musl loader')
            return interpreter[:-1].decode()
    raise ValueError('Binary is missing the musl interpreter')


def package(version, revision, repository, output):
    if not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('Expected vMAJOR.MINOR.PATCH')
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('Expected a full Git commit SHA')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Expected owner/repository')
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise ValueError('This release package supports Linux x86_64 only')

    prefix = f'openmktr-{version}'
    source = output / f'{prefix}-source.zip'
    files = {'bin/omktr': (ROOT / 'omktr').read_bytes()}
    with zipfile.ZipFile(source) as archive:
        for name in LICENSE_FILES:
            data = archive.read(f'{prefix}/{name}')
            if data != (ROOT / name).read_bytes():
                raise ValueError(f'Source archive differs from build tree: {name}')
            files[name] = data
        if archive.read(f'{prefix}/src/main.cc') != (ROOT / 'src/main.cc').read_bytes():
            raise ValueError('Source archive differs from build tree: src/main.cc')

    binary = ROOT / 'omktr'
    version_output = subprocess.check_output([str(binary), '--version'], text=True)
    if f'{version}+{revision}' not in version_output:
        raise ValueError('Binary revision does not match release source')
    interpreter = musl_interpreter(files['bin/omktr'])
    dependencies = subprocess.check_output(['ldd', str(binary)], text=True)
    if any(name in dependencies for name in ('libstdc++', 'libgcc_s', 'libc.so.6')):
        raise ValueError('Binary has an unwanted GNU runtime dependency')
    source_url = f'https://github.com/{repository}/releases/download/{version}/{source.name}'
    readme = f'''Openmktr {version} for Linux x86_64 with musl
Source commit: {revision}

This dynamically linked binary was built and tested in the pinned Chimera
image from Dockerfile. It requires musl and compatible LLVM libc++ runtime
libraries. It is not a standalone binary for glibc Linux. Install the matching
runtime libraries from your musl distribution. Ninja is required for generated
graphs; recipes use the shell and tools supplied by your Makefile.

Extract this archive, then run ./{prefix}/bin/omktr --version. Keep the accompanying
LICENSE, NOTICE, LICENSES/, and docs/LICENSING.md when redistributing it.
Fork contributions use MPL 2.0; upstream material retains Apache 2.0 terms.
Corresponding source is available alongside this binary at:
{source_url}
The source can also be obtained from commit {revision} at:
https://github.com/{repository}/tree/{revision}

Build-time runtime dependencies (paths refer to the test container):
{dependencies}
'''
    files['README.txt'] = readme.encode()
    files['runtime-dependencies.txt'] = (
        f'ELF interpreter: {interpreter}\n{dependencies}').encode()
    binary_archive = output / f'{prefix}-linux-x86_64-musl.tar.gz'
    with tarfile.open(binary_archive, 'w:gz') as archive:
        for name, data in files.items():
            info = tarfile.TarInfo(f'{prefix}/{name}')
            info.size = len(data)
            info.mode = 0o755 if name == 'bin/omktr' else 0o644
            archive.addfile(info, io.BytesIO(data))

    # Verify the distributed bytes and executable permissions before upload.
    with tarfile.open(binary_archive) as archive:
        for name, data in files.items():
            member = archive.getmember(f'{prefix}/{name}')
            if archive.extractfile(member).read() != data:
                raise ValueError(f'Archive verification failed: {name}')
            if name == 'bin/omktr' and member.mode != 0o755:
                raise ValueError('Archived binary is not executable')
    checksums = ''.join(
        f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n'
        for path in (source, binary_archive)
    )
    (output / 'SHA256SUMS').write_text(checksums, encoding='utf-8')
    (output / 'release-notes.md').write_text(
        f'Openmktr {version} reads Makefiles, executes targets in parallel, '
        'and generates Ninja build graphs.\n\n'
        f'Source revision: `{revision}`.\n\n'
        'Assets include the versioned source ZIP, a Linux x86_64 musl binary '
        'archive with licenses and source-access information, and SHA256SUMS. '
        'The binary requires compatible musl and LLVM libc++ runtime libraries; '
        'see README.txt and runtime-dependencies.txt inside the archive.\n\n'
        'The release pipeline passed the Chimera unit tests, version generator, '
        'correctness, Make compatibility regressions, direct/Ninja snapshot '
        'regressions, include-dump smoke test, and ASan/UBSan checks before '
        'packaging. Other platforms are best effort; Makefile compatibility '
        'depends on the project and build mode.\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.version, args.revision, args.repository, args.output)
