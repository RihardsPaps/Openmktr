#!/usr/bin/env bash
set -euo pipefail

target=/root/universal-tool-campaign-20260927/builds/yocto-5.0.10/tmp/work/x86_64-linux/perl-native/5.38.4
[[ "$(realpath "$target")" == "$target" ]]
before=$(find "$target" -xdev -user root | wc -l)
find "$target" -xdev -user root -exec chown yocto:yocto {} +
after=$(find "$target" -xdev -user root | wc -l)
printf 'root-owned entries: %s before, %s after\n' "$before" "$after"
