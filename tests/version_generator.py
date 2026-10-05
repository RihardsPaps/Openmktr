#!/usr/bin/env python3
"""Check version generation in archive builds without a Git executable."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


GENERATOR = Path(__file__).resolve().parents[1] / "tools" / "gen_version.py"


class VersionGenerator(unittest.TestCase):
    def generate(self, revision):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "version.cc"
            env = dict(os.environ, PATH="", OMKTR_SOURCE_REVISION=revision)
            subprocess.run([sys.executable, str(GENERATOR), str(target)],
                           env=env, check=True, capture_output=True)
            content = target.read_text(encoding="utf-8")
            stamp = target.stat().st_mtime_ns
            subprocess.run([sys.executable, str(GENERATOR), str(target)],
                           env=env, check=True, capture_output=True)
            self.assertEqual(target.stat().st_mtime_ns, stamp)
            return content

    def test_archive_without_git(self):
        self.assertEqual(self.generate(""),
                         'const char* kGitVersion = "unversioned";\n')

    def test_explicit_revision_without_git(self):
        self.assertEqual(self.generate('release-"test"'),
                         'const char* kGitVersion = "release-\\"test\\"";\n')


if __name__ == "__main__":
    unittest.main()
