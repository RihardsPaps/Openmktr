"""Compile and run small programs with every installed GCC 14 frontend."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile


prefix = Path(sys.argv[1])
environment = dict(os.environ)
environment["PATH"] = f"{prefix / 'bin'}:{environment.get('PATH', '')}"
environment["LD_LIBRARY_PATH"] = (
    f"{prefix / 'lib64'}:{prefix / 'lib'}:"
    f"{environment.get('LD_LIBRARY_PATH', '')}"
)


def run(*argv):
    result = subprocess.run(argv, cwd=temp, env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, argv))}:\n{result.stdout}")


with tempfile.TemporaryDirectory(prefix="gcc-all-smoke-") as name:
    temp = Path(name)

    def check(label, source_name, source, compiler, *flags, execute=True):
        (temp / source_name).write_text(source)
        output = temp / (label if execute else label + ".o")
        args = [str(prefix / "bin" / compiler), *flags, source_name,
                "-o", str(output)]
        if not execute:
            args.insert(-3, "-c")
        run(*args)
        if execute:
            run(str(output))
        print(f"{label}: PASS", flush=True)

    check("c", "c.c", "int main(void) { return 0; }\n", "gcc")
    check("cxx", "cxx.cc", "int main() { return 0; }\n", "g++")
    check("fortran", "fortran.f90",
          "program smoke\nif (2 + 3 /= 5) stop 1\nend program\n",
          "gfortran")
    check("go", "go.go", "package main\nfunc main() {}\n", "gccgo")
    check("d", "d.d", "void main() {}\n", "gdc")
    check("objc", "objc.m",
          "@interface Fixture @end\n@implementation Fixture @end\n",
          "gcc", execute=False)
    check("objcxx", "objcxx.mm",
          "@interface Fixture @end\n@implementation Fixture @end\n",
          "g++", execute=False)
    check("m2", "m2.mod", "MODULE m2;\nBEGIN\nEND m2.\n",
          "gm2", execute=False)
    check("rust", "rust.rs", "fn main() {}\n", "gccrs",
          "-frust-incomplete-and-experimental-compiler-do-not-use",
          execute=False)
    check("lto", "lto.c", "int main(void) { return 0; }\n",
          "gcc", "-flto")

    (temp / "smoke_ada.adb").write_text(
        "procedure Smoke_Ada is\nbegin\n   null;\nend Smoke_Ada;\n")
    run(str(prefix / "bin" / "gnatmake"), "-q", "smoke_ada.adb", "-o",
        str(temp / "smoke_ada"))
    run(str(temp / "smoke_ada"))
    print("ada: PASS", flush=True)

    library = ctypes.CDLL(str(prefix / "lib" / "libgccjit.so"))
    library.gcc_jit_context_acquire.restype = ctypes.c_void_p
    context = library.gcc_jit_context_acquire()
    if not context:
        raise RuntimeError("libgccjit did not create a context")
    library.gcc_jit_context_release.argtypes = [ctypes.c_void_p]
    library.gcc_jit_context_release(context)
    print("jit: PASS", flush=True)
