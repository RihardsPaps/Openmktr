#!/usr/bin/env python3
"""Exercise a built Git with commits, clone, history, and file retrieval."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    git = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="omktr-git-") as directory:
        root = Path(directory)
        origin = root / "origin"
        clone = root / "clone"

        def run(*args: str, cwd: Path = root) -> str:
            result = subprocess.run(
                [git, *args], cwd=cwd, text=True, capture_output=True, check=True,
            )
            return result.stdout.strip()

        run("init", "-q", str(origin))
        run("config", "user.name", "Validation", cwd=origin)
        run("config", "user.email", "validation@example.invalid", cwd=origin)
        (origin / "data.txt").write_text("first\n")
        run("add", "data.txt", cwd=origin)
        run("commit", "-qm", "first", cwd=origin)
        (origin / "data.txt").write_text("second\n")
        run("commit", "-qam", "second", cwd=origin)
        run("clone", "-q", str(origin), str(clone))
        assert (clone / "data.txt").read_text() == "second\n"
        assert run("log", "-2", "--format=%s", cwd=clone).splitlines() == [
            "second", "first"
        ]
        assert run("show", "HEAD^:data.txt", cwd=clone) == "first"
        assert run("status", "--porcelain", cwd=clone) == ""
        print(f"{run('--version')}; commit/clone/history/status PASS")


if __name__ == "__main__":
    main()
