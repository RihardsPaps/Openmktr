"""Recheck GNU tar issue #20 in a Linux environment with Clang and Ninja.

Usage: python3 validation/reproduce_issue20.py /path/to/tar-1.35.tar.gz /path/to/ckati
Retains fresh extracted sources, builds, and complete command logs for diagnosis.
"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("archive", type=Path, help="GNU tar 1.35 release archive")
parser.add_argument("ckati", type=Path, help="Kati executable to test")
args = parser.parse_args()
archive = args.archive.resolve()
tool = args.ckati.resolve()
root = Path(tempfile.mkdtemp(prefix="kati-issue20-"))
env = os.environ.copy()
# Keep inherited build state from influencing this clean reproduction.
for name in ("MAKEFLAGS", "MAKEOVERRIDES", "MAKELEVEL", "MFLAGS",
             "KATI_JOBS", "KATI_JOBSERVER_FIFO", "KATI_DEPTH"):
    env.pop(name, None)
env.update(MAKE=str(tool), CC="clang", CXX="clang++", FORCE_UNSAFE_CONFIGURE="1")
counter = 0


def run(cwd, label, *command, expected=0):
    global counter
    counter += 1
    log = root / f"{counter:02d}-{label}.log"
    with log.open("w") as output:
        output.write(f"cwd: {cwd}\nargv: {command!r}\n")
        output.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT, timeout=300, check=False)
        output.write(f"\nexit status: {result.returncode}\n")
    print(f"{label}: exit={result.returncode}, log={log}", flush=True)
    if expected is None:
        assert result.returncode != 0, log
    elif result.returncode != expected:
        raise RuntimeError(f"Expected exit {expected}: {log}")
    return log.read_text()


print(f"Evidence directory: {root}", flush=True)
with archive.open("rb") as stream:
    digest = hashlib.file_digest(stream, "sha256").hexdigest()
print(f"Archive SHA256: {digest}", flush=True)
run(root, "version", str(tool), "--version")

for layout in ("in-source", "out-of-source"):
    case = root / layout
    case.mkdir()
    with tarfile.open(archive) as stream:
        stream.extractall(case, filter="data")
    source = case / "tar-1.35"
    build = source if layout == "in-source" else case / "build"
    build.mkdir(exist_ok=True)
    configure = "./configure" if layout == "in-source" else str(source / "configure")
    run(build, f"{layout}-configure", configure)
    assert (build / "po/Makefile").is_file()
    run(build, f"{layout}-convert", str(tool), "--ninja", "--regen",
        "CC=clang", "HOSTCC=clang", "HOSTCXX=clang++", "-f", "Makefile", "all")
    run(build, f"{layout}-parallel", "sh", "ninja.sh", "-j12", "all")
    run(build, f"{layout}-repeat", "sh", "ninja.sh", "-j12", "all")
    version = run(build, f"{layout}-tar-version", "./src/tar", "--version")
    assert "tar (GNU tar) 1.35" in version

    # The opaque recursive edge must find and rebuild missing child outputs.
    (build / "src/tar").unlink()
    run(build, f"{layout}-serial-relink", "sh", "ninja.sh", "-j1", "all")
    assert (build / "src/tar").is_file()
    os.utime(source / "src/tar.c", None)
    run(build, f"{layout}-incremental", "sh", "ninja.sh", "-j12", "all")
    run(build, f"{layout}-regenerate", str(tool), "--ninja", "--regen",
        "CC=clang", "HOSTCC=clang", "HOSTCXX=clang++", "-f", "Makefile", "all")

    # Capture the child error hidden by the issue's abbreviated FAILED line.
    missing = build / "po/Makefile"
    saved = build / "po/Makefile.issue20-saved"
    missing.rename(saved)
    try:
        failure = run(build, f"{layout}-missing-po-makefile", "sh", "ninja.sh",
                      "-j12", "all", expected=None)
        assert "FAILED:" in failure and "Making all in po" in failure, failure
        assert "no makefile found" in failure, failure
    finally:
        saved.rename(missing)
    run(build, f"{layout}-recovered", "sh", "ninja.sh", "-j12", "all")

print(f"GNU tar 1.35 follow-up checks passed; evidence retained at {root}")
