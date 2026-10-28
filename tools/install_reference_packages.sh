#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Pin the complete GNU-free reference toolchain, including base-image upgrades.
set -eu
lock=${1:-tools/chimera-packages.lock}
packages=$(sed '/^#/d; /^$/d' "$lock")
# Word splitting is intentional: each lock entry is one apk package constraint.
if ! apk add --no-cache --upgrade $packages; then
    printf '%s\n' 'Reference packages unavailable; refresh the reviewed dependency lock.' >&2
    exit 1
fi
python3 tools/check_reference_packages.py "$lock"
