"""Debug a freshly compiled C program with the installed GDB."""

from pathlib import Path
import subprocess
import sys
import tempfile


gdb = Path(sys.argv[1]).resolve() / "bin" / "gdb"
with tempfile.TemporaryDirectory(prefix="gdb-verify-") as temporary:
    root = Path(temporary)
    source = root / "program.c"
    program = root / "program"
    source.write_text("__attribute__((noinline)) int mark(int value) { return value; }\n"
                      "int main(void) { return mark(12); }\n")
    subprocess.run(["clang", "-g", "-O0", str(source), "-o", str(program)],
                   check=True, capture_output=True, text=True, timeout=30)
    result = subprocess.run(
        [str(gdb), "--batch", "--quiet", "-ex", "break mark",
         "-ex", "run", "-ex", "print value", str(program)],
        capture_output=True, text=True, check=True, timeout=60,
    )
    assert "Breakpoint" in result.stdout, result.stdout
    assert "$1 = 12" in result.stdout, result.stdout
    print(result.stdout)
    print("Installed GDB breakpoint and variable inspection PASS")
