"""Exercise installed ImageMagick coders and transforms."""
from pathlib import Path
import subprocess
import sys
import tempfile


magick = Path(sys.argv[1]).resolve() / "bin/magick"
with tempfile.TemporaryDirectory(prefix="imagemagick-smoke-") as tmp:
    tmp = Path(tmp)
    png = tmp / "image.png"
    jpeg = tmp / "small.jpg"
    subprocess.run([magick, "-size", "16x16", "xc:red", "-fill", "blue",
                    "-draw", "rectangle 8,0 15,15", png], check=True)
    subprocess.run([magick, png, "-resize", "8x8!", jpeg], check=True)
    for image, expected in ((png, "PNG 16x16"), (jpeg, "JPEG 8x8")):
        result = subprocess.run([magick, "identify", "-format", "%m %wx%h",
                                 image], check=True, capture_output=True, text=True)
        assert result.stdout == expected, (image, result.stdout)
    print("PNG generation and JPEG resize/identify PASS")
