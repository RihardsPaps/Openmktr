#!/usr/bin/env python3
"""Compile and run a small off-screen rendering test against installed Mesa."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    prefix = Path(sys.argv[1]).resolve()
    source = r"""
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <stdio.h>

int main(void) {
    unsigned char framebuffer[4 * 4 * 4] = {0};
    unsigned char pixel[4] = {0};
    OSMesaContext context = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
    if (!context || !OSMesaMakeCurrent(context, framebuffer, GL_UNSIGNED_BYTE, 4, 4))
        return 1;
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("OpenGL %s; pixel=%u,%u,%u,%u\n", glGetString(GL_VERSION),
           pixel[0], pixel[1], pixel[2], pixel[3]);
    OSMesaDestroyContext(context);
    return pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255
               ? 0 : 2;
}
"""
    with tempfile.TemporaryDirectory(prefix="mesa-osmesa-smoke-") as temp:
        root = Path(temp)
        (root / "smoke.c").write_text(source, encoding="utf-8")
        subprocess.run(
            ["cc", str(root / "smoke.c"), "-I" + str(prefix / "include"),
             "-L" + str(prefix / "lib"), "-Wl,-rpath," + str(prefix / "lib"),
             "-lOSMesa", "-o", str(root / "smoke")],
            check=True,
        )
        environment = dict(os.environ)
        environment["LD_LIBRARY_PATH"] = str(prefix / "lib")
        subprocess.run([str(root / "smoke")], env=environment, check=True)


if __name__ == "__main__":
    main()
