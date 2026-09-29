#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
sets="$campaign/sources/netbsd-10.1-sets"
source_dir="$campaign/sources/netbsd-10.1"
mkdir -p "$source_dir"
for name in src syssrc gnusrc sharesrc; do
    expected=$(sed -n "s/^SHA512 ($name.tgz) = //p" "$sets/SHA512")
    actual=$(sha512sum "$sets/$name.tgz" | cut -d ' ' -f 1)
    [[ -n "$expected" && "$expected" == "$actual" ]]
    tar -xzf "$sets/$name.tgz" -C "$source_dir" --strip-components=2
    echo "$name.tgz verified and extracted"
done
test -f "$source_dir/build.sh"
test -f "$source_dir/Makefile"
