"""Link and run an Xlib/Xext client against the selected installed modules."""
from pathlib import Path
import subprocess
import sys
import tempfile


prefix = Path(sys.argv[1])
source = r"""
#include <locale.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/extensions/extutil.h>
int main(void) {
    if (!setlocale(LC_ALL, "C") || !XInitThreads() || !XSupportsLocale())
        return 1;
    XExtensionInfo *ext = XextCreateExtension();
    if (!ext) return 2;
    XextDestroyExtension(ext);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="xorg-smoke-") as temp:
    program = Path(temp) / "client.c"
    executable = Path(temp) / "client"
    program.write_text(source)
    subprocess.run(["gcc", str(program), f"-I{prefix / 'include'}",
                    f"-L{prefix / 'lib'}", f"-Wl,-rpath,{prefix / 'lib'}",
                    "-lX11", "-lXext", "-o", str(executable)], check=True)
    bindings = subprocess.check_output(["ldd", str(executable)], text=True)
    for library in ("libX11.so", "libXext.so"):
        if library not in bindings or str(prefix / "lib") not in bindings:
            raise SystemExit(f"installed {library} not linked")
    subprocess.run([str(executable)], check=True)
    print("Installed Xlib and Xext linked and passed client runtime smoke")
