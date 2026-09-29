"""Generate, build, test, and install a small Libtool/Automake project."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


base = Path(sys.argv[1]).resolve()
tool = base / "tool" / "ckati"
prefix = base / "install" / "autotools"
root = Path(tempfile.mkdtemp(prefix="autotools-verify-", dir=base / "builds"))
source = root / "source"
build = root / "build"
install = root / "install"
source.mkdir()
build.mkdir()
source.joinpath("configure.ac").write_text(
    "AC_INIT([campaign-sample], [1.0])\n"
    "AM_INIT_AUTOMAKE([foreign])\n"
    "AC_PROG_CC\nLT_INIT\n"
    "AC_CONFIG_FILES([Makefile])\nAC_OUTPUT\n")
source.joinpath("Makefile.am").write_text(
    "lib_LTLIBRARIES = libsample.la\n"
    "libsample_la_SOURCES = sample.c\n"
    "bin_PROGRAMS = sample\n"
    "sample_SOURCES = main.c\n"
    "sample_LDADD = libsample.la\n"
    "TESTS = sample\n")
source.joinpath("sample.c").write_text("int sample(void) { return 42; }\n")
source.joinpath("main.c").write_text(
    "extern int sample(void);\nint main(void) { return sample() != 42; }\n")
env = os.environ.copy()
env["PATH"] = f"{prefix / 'bin'}:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
env["MAKE"] = str(tool)
env["CC"] = "clang"
env["CFLAGS"] = "-O2"


def run(cwd, *command):
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180, check=False)
    print(f"$ {' '.join(map(str, command))}\n{result.stdout}", flush=True)
    if result.returncode:
        raise RuntimeError(f"command exited {result.returncode}")


try:
    run(source, "autoreconf", "-fi")
    run(build, str(source / "configure"), f"--prefix={install}")
    run(build, str(tool), "-j2", f"MAKE={tool}", "check")
    run(build, str(tool), "-j2", f"MAKE={tool}", "install")
    run(build, str(tool), "-j2", f"MAKE={tool}")
    run(build, str(install / "bin" / "sample"))
    print("Pinned Autotools generated-project build/test/install PASS")
except Exception:
    print(f"Autotools diagnostic project retained at {root}", file=sys.stderr)
    raise
else:
    shutil.rmtree(root)
