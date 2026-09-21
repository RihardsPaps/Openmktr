#!/bin/bash

# Exercise .DELETE_ON_ERROR through both GNU make and generated Ninja.  The
# script is intentionally target-agnostic: it checks the generic output
# cleanup contract rather than any project-specific recipe.
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

if [[ "$mk" == *ckati* ]]; then
	(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1) || true
	(cd "$tmp" && ./ninja.sh -j1 test >/dev/null 2>&1) || true
else
	make -C "$tmp" test >/dev/null 2>&1 || true
fi

if [ -e "$tmp/test" ]; then
	echo ".DELETE_ON_ERROR left a failed output behind" >&2
	exit 1
fi
