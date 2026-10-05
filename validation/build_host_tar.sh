#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
archive="$campaign/downloads/tar-1.34.tar.xz"
source="$campaign/sources/tar-1.34"
build="$campaign/builds/tar-1.34"
prefix="$campaign/host-deps/tar-1.34"

mkdir -p "$campaign/downloads" "$campaign/sources" "$build" "$prefix"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 --output "$archive" \
    https://mirrors.kernel.org/gnu/tar/tar-1.34.tar.xz
fi
sha256sum "$archive"
if [[ ! -d "$source" ]]; then
  tar -xJf "$archive" -C "$campaign/sources"
fi
if [[ ! -f "$build/Makefile" ]]; then
  (cd "$build" && FORCE_UNSAFE_CONFIGURE=1 "$source/configure" --prefix="$prefix" --disable-nls)
fi
"$campaign/tool/omktr" -C "$build" -j2
"$campaign/tool/omktr" -C "$build" -j2 install
"$prefix/bin/tar" --version | head -1
