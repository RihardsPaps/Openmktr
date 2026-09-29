#!/usr/bin/env bash
set -euo pipefail

config=/root/universal-tool-campaign-20260927/builds/yocto-5.0.10/conf/local.conf
old='EXTRA_OEMAKE:pn-elfutils = "-j1"'
setting='PTEST_PARALLEL_MAKE:pn-elfutils = "-j1"'
if grep -Fxq "$old" "$config"; then
  sed -i "s/^EXTRA_OEMAKE:pn-elfutils = \"-j1\"$/PTEST_PARALLEL_MAKE:pn-elfutils = \"-j1\"/" "$config"
fi
grep -Fxq "$setting" "$config" || printf '\n# Serialize elfutils ptest goals that build the same objects concurrently.\n%s\n' "$setting" >> "$config"
grep -F "$setting" "$config"
