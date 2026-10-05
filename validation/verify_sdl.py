"""Build and run an SDL3 application with the dummy video backend."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


prefix = Path(sys.argv[1]).resolve()
env = os.environ.copy()
env["PKG_CONFIG_PATH"] = f"{prefix / 'lib/pkgconfig'}:{env.get('PKG_CONFIG_PATH', '')}"
env["LD_LIBRARY_PATH"] = f"{prefix / 'lib'}:{env.get('LD_LIBRARY_PATH', '')}"
env["SDL_VIDEODRIVER"] = "dummy"
source = """
#include <SDL3/SDL.h>
int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_Window *window = SDL_CreateWindow("omktr smoke", 32, 32, SDL_WINDOW_HIDDEN);
  if (!window) return 2;
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="sdl-smoke-") as tmp:
    src = Path(tmp) / "smoke.c"
    binary = Path(tmp) / "smoke"
    src.write_text(source)
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl3"],
                           check=True, capture_output=True, text=True,
                           env=env).stdout
    subprocess.run(["cc", str(src), "-o", str(binary), *shlex.split(flags)],
                   check=True, env=env)
    subprocess.run([binary], check=True, env=env)
    print("Installed SDL3 compiled, linked, and initialized dummy video PASS")
