"""Exercise the installed VLC as an unprivileged user with a generated WAV."""
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


prefix = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix="vlc-smoke-") as temp:
    sample = Path(temp) / "tone.wav"
    with wave.open(str(sample), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        samples = [int(7000 * math.sin(2 * math.pi * 440 * n / 8000))
                   for n in range(4000)]
        wav.writeframes(struct.pack("<" + "h" * len(samples), *samples))
    os.chmod(temp, 0o755)
    os.chmod(sample, 0o644)

    # VLC deliberately refuses root.  Grant only directory traversal while
    # its unprivileged smoke process runs, and remove that ACL even on failure.
    subprocess.run(["setfacl", "-m", "u:nobody:x", "/root"], check=True)
    try:
        command = ["runuser", "-u", "nobody", "--", "env", "HOME=/tmp",
                   f"VLC_PLUGIN_PATH={prefix / 'lib/vlc/plugins'}",
                   str(prefix / "bin/vlc"), "--intf", "dummy", "--aout",
                   "dummy", "--no-video", "--play-and-exit", "-vv",
                   str(sample)]
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
    finally:
        subprocess.run(["setfacl", "-x", "u:nobody", "/root"], check=True)
    print(result.stdout[-5000:])
    if (result.returncode or "successfully opened" not in result.stdout or
            'removing module "wav"' not in result.stdout or
            "end of playlist, exiting" not in result.stdout):
        raise SystemExit(f"VLC playback failed: exit {result.returncode}")
    print("Installed VLC decoded and played the generated WAV as nobody")
