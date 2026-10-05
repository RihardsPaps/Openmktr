#!/usr/bin/env bash
set -eo pipefail

campaign=/root/universal-tool-campaign-20260927
source "$campaign/sources/poky-5.0.10/oe-init-build-env" \
  "$campaign/builds/yocto-5.0.10" >/dev/null

python3 - "$campaign" <<'PY'
from pathlib import Path
import sys

campaign = Path(sys.argv[1])
config = Path("conf/local.conf")
block = f"""
# Campaign-specific settings, confined to the separate build directory.
MACHINE = "qemux86-64"
BB_NUMBER_THREADS = "2"
PARALLEL_MAKE = "-j2"
MAKE = "{campaign}/tool/omktr"
DL_DIR = "{campaign}/downloads/yocto-5.0.10"
SSTATE_DIR = "{campaign}/sstate/yocto-5.0.10"
"""
text = config.read_text()
if block in text:
    first = text.index(block) + len(block)
    text = text[:first] + text[first:].replace(block, "")
else:
    text += block
config.write_text(text)
PY

bitbake --version
