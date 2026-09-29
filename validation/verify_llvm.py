"""Check installed Clang, lld, LLVM tools, and the configured target registry."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser()
parser.add_argument("prefix", type=Path)
args = parser.parse_args()
bin_dir = args.prefix.resolve() / "bin"
env = os.environ.copy()
env["PATH"] = f"{bin_dir}:{env['PATH']}"

def run(*args, **kwargs):
    return subprocess.run([str(bin_dir / args[0]), *map(str, args[1:])],
                          check=True, env=env, **kwargs)

with tempfile.TemporaryDirectory(prefix="llvm-smoke-") as tmp:
    tmp = Path(tmp)
    c = tmp / "smoke.c"
    cpp = tmp / "smoke.cpp"
    c.write_text("int main(void) { return 7 * 6 != 42; }\n")
    cpp.write_text("#include <vector>\nint main() { std::vector<int> v{42}; return v[0] != 42; }\n")
    for compiler, source, out in (("clang", c, "c-smoke"),
                                  ("clang++", cpp, "cpp-smoke")):
        binary = tmp / out
        run(compiler, "-fuse-ld=lld", source, "-o", binary)
        subprocess.run([binary], check=True)
    bitcode = tmp / "smoke.bc"
    assembly = tmp / "smoke.ll"
    object_file = tmp / "smoke.o"
    run("clang", "-emit-llvm", "-c", c, "-o", bitcode)
    run("llvm-dis", bitcode, "-o", assembly)
    run("llvm-as", assembly, "-o", bitcode)
    run("llc", "-filetype=obj", bitcode, "-o", object_file)
    assert object_file.stat().st_size > 0
    targets = run("llc", "--version", capture_output=True, text=True).stdout
    assert "Registered Targets:" in targets
    for target in ("aarch64", "arm", "riscv32", "riscv64", "wasm32", "x86", "x86-64"):
        assert target in targets, target
    print("C/C++ via clang+lld, bitcode round trip, llc object, and all-target registry passed")
