#!/bin/bash

# A binary GNU Make extension cannot be translated without embedding a
# foreign extension ABI. Kati must reject it clearly rather than generating a
# graph that silently omits the extension's effects.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
load example-extension
all:
	@true
EOF

if (cd "$tmp" && "$mk" -f Makefile all >out 2>&1); then
  exit 1
fi
grep -F 'load: dynamic make extensions are unsupported by parallel Kati' \
  "$tmp/out" >/dev/null

if (cd "$tmp" && "$mk" --ninja --regen -f Makefile >out-ninja 2>&1); then
  exit 1
fi
grep -F 'load: dynamic make extensions are unsupported by parallel Kati' \
  "$tmp/out-ninja" >/dev/null
