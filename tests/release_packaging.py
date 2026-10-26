#!/usr/bin/env python3
"""Check release archive contents and rejection of incompatible binaries.

Copyright 2026 Rihards Paps and Haralds Paps
SPDX-License-Identifier: MPL-2.0
"""

import hashlib
import importlib.util
from pathlib import Path
import re
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SPEC = importlib.util.spec_from_file_location(
    'package_release', Path(__file__).resolve().parents[1] / 'tools/package_release.py')
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)
VERSION = 'v0.1.0'
REVISION = 'a' * 40


def elf_binary(loader=b'/lib/ld-musl-x86_64.so.1\0'):
    data = bytearray(120)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', data, 18, 62)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 1)
    struct.pack_into('<I', data, 64, 3)
    struct.pack_into('<Q', data, 72, 120)
    struct.pack_into('<Q', data, 96, len(loader))
    return bytes(data) + loader


class ReleasePackagingTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / 'dist'
        self.output.mkdir()
        for name in (*RELEASE.LICENSE_FILES, 'src/main.cc', *RELEASE.SUPPORT_FILES.values()):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f'fixture: {name}', encoding='utf-8')
        (self.root / 'omktr').write_bytes(elf_binary())
        self.source = self.output / f'openmktr-{VERSION}-source.zip'
        self.write_source()
        patcher = patch.object(RELEASE, 'ROOT', self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        for target, value in (('system', 'Linux'), ('machine', 'x86_64')):
            patcher = patch.object(RELEASE.platform, target, return_value=value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.version_output = f'Openmktr {VERSION}+{REVISION}\n'
        self.dependencies = '/lib/ld-musl-x86_64.so.1\nlibc++.so.1\n'
        patcher = patch.object(RELEASE.subprocess, 'check_output', side_effect=self.command)
        patcher.start()
        self.addCleanup(patcher.stop)

    def write_source(self, corrupt_license=False):
        with zipfile.ZipFile(self.source, 'w') as archive:
            for name in (*RELEASE.LICENSE_FILES, 'src/main.cc', *RELEASE.SUPPORT_FILES.values()):
                data = (self.root / name).read_bytes()
                if corrupt_license and name == 'LICENSE':
                    data = b'wrong license'
                archive.writestr(f'openmktr-{VERSION}/{name}', data)

    def command(self, args, **kwargs):
        return self.dependencies if args[0] == 'ldd' else self.version_output

    def package(self):
        RELEASE.package(VERSION, REVISION, 'owner/repo', self.output)

    def test_complete_distribution(self):
        self.package()
        binary_archive = self.output / f'openmktr-{VERSION}-linux-x86_64-musl.tar.gz'
        with tarfile.open(binary_archive) as archive:
            for name in RELEASE.LICENSE_FILES:
                self.assertEqual(archive.extractfile(f'openmktr-{VERSION}/{name}').read(),
                                 (self.root / name).read_bytes())
            binary = archive.getmember(f'openmktr-{VERSION}/bin/omktr')
            self.assertEqual(binary.mode, 0o755)
            self.assertEqual(archive.extractfile(binary).read(), elf_binary())
            helper = archive.getmember(f'openmktr-{VERSION}/bin/omktr-workspace')
            self.assertEqual(helper.mode, 0o755)
            self.assertEqual(archive.extractfile(helper).read(),
                             (self.root / 'tools/omktr_workspace.py').read_bytes())
            readme = archive.extractfile(f'openmktr-{VERSION}/README.txt').read().decode()
            self.assertIn(f'/releases/download/{VERSION}/{self.source.name}', readme)
            self.assertIn('dynamically linked', readme)
            command_path = re.search(r'run \./(\S+) --version', readme).group(1)
            self.assertEqual(archive.getmember(command_path), binary)
        checksums = (self.output / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(checksums), 2)
        for line in checksums:
            digest, name = line.split('  ')
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_reject_mismatched_license(self):
        self.write_source(corrupt_license=True)
        with self.assertRaisesRegex(ValueError, 'LICENSE'):
            self.package()

    def test_reject_mismatched_revision(self):
        self.version_output = 'unversioned'
        with self.assertRaisesRegex(ValueError, 'revision'):
            self.package()

    def test_reject_glibc_loader(self):
        (self.root / 'omktr').write_bytes(elf_binary(b'/lib64/ld-linux-x86-64.so.2\0'))
        with self.assertRaisesRegex(ValueError, 'musl loader'):
            self.package()

    def test_reject_gnu_runtime(self):
        self.dependencies += 'libgcc_s.so.1\n'
        with self.assertRaisesRegex(ValueError, 'GNU runtime'):
            self.package()

    def test_reject_invalid_version(self):
        with self.assertRaisesRegex(ValueError, 'vMAJOR'):
            RELEASE.package('../bad', REVISION, 'owner/repo', self.output)


if __name__ == '__main__':
    unittest.main()
