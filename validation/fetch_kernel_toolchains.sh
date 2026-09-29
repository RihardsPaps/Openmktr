#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
base=https://www.kernel.org/pub/tools/crosstool/files/bin/x86_64/13.3.0
downloads="$campaign/downloads/crosstool-13.3.0"
destination="$campaign/host-deps"
mkdir -p "$downloads" "$destination"
manifest="$downloads/sha256sums.asc"
if [[ ! -f "$manifest" ]]; then
  curl --fail --location --retry 3 --silent --show-error \
    --output "$manifest" "$base/sha256sums.asc"
fi

for arch in "${@:-csky}"; do
  case "$arch" in
    csky|microblaze|nios2|or1k|xtensa) ;;
    *) echo "unsupported requested toolchain: $arch" >&2; exit 2 ;;
  esac
  name="x86_64-gcc-13.3.0-nolibc-$arch-linux.tar.xz"
  archive="$downloads/$name"
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 --silent --show-error \
      --output "$archive" "$base/$name"
  fi
  (cd "$downloads" && grep -F "  $name" "$manifest" | sha256sum -c -)
  if [[ ! -x "$destination/gcc-13.3.0-nolibc/$arch-linux/bin/$arch-linux-gcc" ]]; then
    tar -xJf "$archive" -C "$destination"
  fi
  "$destination/gcc-13.3.0-nolibc/$arch-linux/bin/$arch-linux-gcc" \
    -dumpfullversion
done
