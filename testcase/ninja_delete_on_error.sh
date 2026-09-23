#!/bin/sh

# Exercise .DELETE_ON_ERROR through generated Ninja.
set -u

mk="$1"
mk=$(realpath "$mk")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat >"$tmp/Makefile" <<'EOF'
.DELETE_ON_ERROR:

test:
	@touch $@
	@false
EOF

(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1) || true
(cd "$tmp" && ./ninja.sh -j1 test >/dev/null 2>&1) || true

if [ -e "$tmp/test" ]; then
	echo ".DELETE_ON_ERROR left a failed output behind" >&2
	exit 1
fi
