#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
build="$campaign/builds/libreoffice-25.2.7.2"
tool="$campaign/tool/omktr"
all_langs=$(sed -n 's/^ALL_LANGS=//p' "$build/config_host_lang.mk")
if [[ -z "$all_langs" ]]; then
    echo "ALL_LANGS missing from configured LibreOffice build" >&2
    exit 1
fi
cd "$build"
exec "$tool" -j2 "MAKE=$tool" "ALL_LANGS=$all_langs" "$@"
