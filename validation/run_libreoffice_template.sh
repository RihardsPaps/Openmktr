#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
build="$campaign/builds/libreoffice-25.2.7.2"
source_dir="$campaign/sources/libreoffice-25.2.7.2"
all_langs=$(sed -n 's/^ALL_LANGS=//p' "$build/config_host_lang.mk")
cd "$build"
exec "$campaign/tool/omktr" -rs -f "$source_dir/Makefile.gbuild" \
    "ALL_LANGS=$all_langs" "SRCDIR=$source_dir" "BUILDDIR=$build" \
    "WORKDIR=$build/workdir" \
    "$build/workdir/ScpTemplateTarget/scp2/source/templates/alllangmodules_base.inc"
