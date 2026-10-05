#!/usr/bin/env python3
"""Clean, pinned x86_64 GNU/Linux bootstrap. Never patches upstream sources.

Run in the prepared Ubuntu container, with validation mounted at /report:
  python3 /report/bootstrap.py
Requires an unused /campaign/clean-* directory set; refuses existing builds.
"""
import hashlib
import argparse
import os
from pathlib import Path
import shutil
import subprocess

from phase import run

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--resume", action="store_true")
parser.add_argument("--start-at", choices=["glibc-configure", "glibc-headers"])
parser.add_argument("--root", type=Path, default=Path("/campaign"))
parser.add_argument("--host-cc", default="gcc")
parser.add_argument("--host-cxx", default="g++")
parser.add_argument("--tool", type=Path, default=Path("/campaign/tool/omktr"))
parser.add_argument("--sources", type=Path, default=Path("/campaign/pinned-sources"))
parser.add_argument("--report", type=Path, default=Path("/report"))
options = parser.parse_args()
if options.start_at and not options.resume:
    parser.error("--start-at requires --resume and preserved earlier phases")
started = options.start_at is None
ROOT = options.root
SOURCES = options.sources
BUILDS = ROOT / "clean-builds"
PREFIX = ROOT / "clean-toolchain"
SYSROOT = ROOT / "clean-sysroot"
KATI = str(options.tool)
JOBS = "8"
RESUME = options.resume
ENV = dict(os.environ, KATI_JOBS=JOBS, CC=options.host_cc,
           CXX=options.host_cxx, CFLAGS="-O2", CXXFLAGS="-O2")
ENV["PATH"] = (f"{PREFIX}/bin:{ROOT}/bin:"
               f"{options.tool.parent.parent}/bin:" + ENV["PATH"])

ARCHIVES = [
    ("binutils-2.44", "https://mirrors.kernel.org/gnu/binutils/binutils-2.44.tar.xz",
     "ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237"),
    ("gcc-14.2.0", "https://mirrors.kernel.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz",
     "a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9"),
    ("glibc-2.40", "https://mirrors.kernel.org/gnu/libc/glibc-2.40.tar.xz",
     "19a890175e9263d748f627993de6f4b1af9cd21e03f080e4bfb3a1fac10205a2"),
    ("linux-6.12", "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz",
     "b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb"),
]


def phase(label, argv, directory=BUILDS):
    global started
    if not started:
        if label != options.start_at:
            return
        started = True
    run("clean-" + label, list(map(str, argv)), str(directory),
        report=options.report, env=ENV)


def make(label, directory, *args):
    phase(label, [KATI, "-j" + JOBS, "MAKE=" + KATI, *args], directory)


def configure(label, source, *args):
    directory = BUILDS / label
    directory.mkdir(exist_ok=RESUME)
    phase(label + "-configure", [SOURCES / source / "configure", *args], directory)
    return directory


if __name__ == "__main__":
    required = [options.host_cc, options.host_cxx, "rsync", "makeinfo", "bison", "flex", "gawk", "curl", "tar"]
    missing = [command for command in required if not shutil.which(command, path=ENV["PATH"])]
    if missing:
        raise SystemExit("Missing host prerequisites: " + ", ".join(missing))
    if not RESUME and any(path.exists() for path in (BUILDS, PREFIX, SYSROOT)):
        raise SystemExit("Refusing reused clean build/output directories")
    BUILDS.mkdir(parents=True, exist_ok=RESUME)
    SOURCES.mkdir(exist_ok=True)
    PREFIX.mkdir(exist_ok=RESUME)
    SYSROOT.mkdir(exist_ok=RESUME)
    for name, url, digest in ARCHIVES:
        archive = SOURCES / (name + ".tar.xz")
        if not archive.exists():
            phase(name + "-download", ["curl", "-fL", "--retry", "3", url,
                                       "-o", archive])
        with archive.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != digest:
            raise SystemExit(f"Digest mismatch: {archive}")
        if not (SOURCES / name).exists():
            phase(name + "-extract", ["tar", "-xf", archive, "-C", SOURCES])
    target = "x86_64-linux-gnu"
    common = ["--target=" + target, "--prefix=" + str(PREFIX),
              "--with-sysroot=" + str(SYSROOT), "--disable-nls", "--disable-werror"]
    binutils = configure("binutils", "binutils-2.44", *common, "--disable-gprofng")
    make("binutils-build", binutils)
    make("binutils-install", binutils, "install")
    headers = BUILDS / "linux-headers"
    make("linux-headers", SOURCES / "linux-6.12", "O=" + str(headers), "ARCH=x86",
         "INSTALL_HDR_PATH=" + str(SYSROOT / "usr"), "headers_install")
    gcc_opts = common + ["--with-build-sysroot=" + str(SYSROOT),
                        "--with-build-time-tools=" + str(PREFIX / target / "bin"),
                        "--disable-bootstrap", "--disable-multilib", "--enable-languages=c,c++"]
    stage1 = configure("gcc-stage1", "gcc-14.2.0", *gcc_opts,
                       "--disable-shared", "--without-headers")
    make("gcc-stage1", stage1, "all-gcc")
    make("gcc-stage1-install", stage1, "install-gcc")
    build = subprocess.check_output(["gcc", "-dumpmachine"], text=True).strip()
    ENV["CC"] = target + "-gcc"
    # Stage one has no target libstdc++ or shared libgcc yet. Disable glibc's
    # optional C++ support helpers until the complete target compiler exists.
    ENV["CXX"] = "false"
    glibc = configure("glibc", "glibc-2.40", "--host=" + target, "--build=" + build,
                      "--prefix=/usr", "--disable-werror", "--enable-kernel=4.19")
    make("glibc-headers", glibc, "install-bootstrap-headers=yes", "install-headers",
         "install_root=" + str(SYSROOT))
    make("glibc-csu", glibc, "csu/subdir_lib")
    lib = SYSROOT / "usr/lib"
    lib.mkdir(parents=True, exist_ok=True)
    for name in ("crt1.o", "crti.o", "crtn.o"):
        shutil.copy2(glibc / "csu" / name, lib / name)
    phase("bootstrap-libc", [PREFIX / "bin" / (target + "-gcc"), "-nostdlib",
                            "-nostartfiles", "-shared", "-x", "c", "/dev/null",
                            "-o", lib / "libc.so"])
    (SYSROOT / "usr/include/gnu/stubs.h").touch()
    ENV["CC"] = options.host_cc
    ENV["CXX"] = options.host_cxx
    make("libgcc", stage1, "all-target-libgcc")
    make("libgcc-install", stage1, "install-target-libgcc")
    make("glibc-build", glibc)
    make("glibc-install", glibc, "install", "install_root=" + str(SYSROOT))
    stage2 = configure("gcc-stage2", "gcc-14.2.0", *gcc_opts)
    make("gcc-stage2", stage2)
    make("gcc-stage2-install", stage2, "install")
    source = BUILDS / "smoke.c"
    source.write_text("#include <stdio.h>\nint main(void){puts(\"bootstrap-ok\");return 0;}\n")
    executable = BUILDS / "smoke"
    phase("smoke-compile", [PREFIX / "bin" / (target + "-gcc"), source, "-o", executable])
    phase("smoke-run", [SYSROOT / "lib64/ld-linux-x86-64.so.2", "--library-path",
                        str(SYSROOT / "lib64") + ":" + str(SYSROOT / "usr/lib64") +
                        ":" + str(SYSROOT / "lib") + ":" + str(SYSROOT / "usr/lib"), executable])
    for name, _, _ in ARCHIVES:
        phase(name + "-source-compare", ["tar", "-df", SOURCES / (name + ".tar.xz"),
                                        "-C", SOURCES])
