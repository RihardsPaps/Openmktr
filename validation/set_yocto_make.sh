#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
case "${1:?pass promoted or candidate}" in
  promoted) tool="$campaign/tool/ckati" ;;
  candidate) tool="$campaign/tool-next2/ckati" ;;
  *) echo "invalid tool selection" >&2; exit 2 ;;
esac

config="$campaign/builds/yocto-5.0.10/conf/local.conf"
sed -i -E "s|^MAKE = \"[^\"]+\"$|MAKE = \"$tool\"|" "$config"
grep '^MAKE = ' "$config"
