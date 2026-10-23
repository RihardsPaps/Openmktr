#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Report semantic differences that constrain automatic child graph reuse.

This is an investigation tool, not an expected-failure regression suite.
An equivalence=false result is evidence against automatically caching that case.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def probe(binary, source, files, observe, mutate):
    results = {}
    for mode in ("direct", "cached"):
        with tempfile.TemporaryDirectory(prefix="omktr-cache-probe-") as temp:
            root = Path(temp)
            (root / "Makefile").write_text(source)
            for name, text in files.items():
                (root / name).write_text(text)
            command = [str(binary), *(["build"] if mode == "cached" else []), "all"]
            for iteration in range(2):
                if iteration:
                    mutate(root)
                result = subprocess.run(command, cwd=root, capture_output=True,
                                        text=True, timeout=15, check=True)
            results[mode] = observe(root, result)
    return {**results, "equivalent": results["direct"] == results["cached"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    observations = {}
    observations["parse_time_info"] = probe(
        binary, "$(info evaluated)\n.PHONY: all\nall:\n\t@:\n", {},
        lambda root, result: "evaluated" in result.stdout,
        lambda root: None)
    observations["generated_include_prerequisite"] = probe(
        binary, "include config.mk\n.PHONY: all\nall:\n"
                "\t@printf '%s' '$(VALUE)' > result\n"
                "config.mk: seed\n\t@cp seed config.mk\n",
        {"seed": "VALUE := one\n", "config.mk": "VALUE := one\n"},
        lambda root, result: (root / "result").read_text(),
        lambda root: (root / "seed").write_text("VALUE := two\n"))
    source = ".PHONY: all\nall:\n\t@printf '%s' 'one' > result\n"

    def backdate(root):
        stamp = (root / "Makefile").stat()
        (root / "Makefile").write_text(source.replace("one", "two"))
        os.utime(root / "Makefile", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))

    observations["preserved_makefile_mtime"] = probe(
        binary, source, {},
        lambda root, result: (root / "result").read_text(), backdate)
    print(json.dumps(observations, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
