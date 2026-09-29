#!/usr/bin/env bash
set -euo pipefail

sets=/root/universal-tool-campaign-20260927/sources/netbsd-10.1-sets
base=https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/source/sets
mkdir -p "$sets"
cd "$sets"
curl -fL --retry 5 -o SHA512 "$base/SHA512"
for name in src syssrc gnusrc sharesrc; do
    if [[ ! -f "$name.tgz" ]]; then
        curl -fL --retry 5 --continue-at - -o "$name.tgz.part" "$base/$name.tgz"
        mv "$name.tgz.part" "$name.tgz"
    fi
    expected=$(sed -n "s/^SHA512 ($name.tgz) = //p" SHA512)
    actual=$(sha512sum "$name.tgz" | cut -d ' ' -f 1)
    [[ -n "$expected" && "$expected" == "$actual" ]]
    echo "$name.tgz SHA512 verified"
done
