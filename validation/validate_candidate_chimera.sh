#!/usr/bin/env bash
set -euo pipefail

workspace='/mnt/c/Users/Harry/Documents/ChatGPT/Universal Tool'
root=/root/universal-tool-campaign-20260927/chimera-root-complete
mountpoint -q "$root/proc" || mount --bind /proc "$root/proc"
mountpoint -q "$root/dev" || mount --bind /dev "$root/dev"
mountpoint -q "$root/report" || mount --bind "$workspace/validation" "$root/report"
cp -R "$workspace/src/." "$root/workspace/src/"
for file in tests/correctness.py tests/make_compat_regressions.py \
            tests/make_compat_regressions.json validation/boot_openwrt.py \
            validation/verify_xorg.py \
            tests/golden.json tests/known_crashes.json \
            tests/version_generator.py tools/gen_version.py; do
    cp "$workspace/$file" "$root/workspace/$file"
done
chroot "$root" /bin/sh -c \
    'cd /workspace && ninja -j1 omktr tests && out/find_test && out/ninja_test && out/strutil_test && python3 tests/version_generator.py && python3 tests/correctness.py -q && python3 tests/make_compat_regressions.py -q && python3 tests/regression.py && sh testcase/dump/run.sh'
