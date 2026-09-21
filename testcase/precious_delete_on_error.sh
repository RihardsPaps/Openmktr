#!/bin/bash

# .PRECIOUS must prevent .DELETE_ON_ERROR from removing a failed output.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat >"$tmp/Makefile" <<'EOF'
.DELETE_ON_ERROR:
.PRECIOUS: test

test:
	@touch $@
	@false
EOF

(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1) || true
(cd "$tmp" && ./ninja.sh -j1 test >/dev/null 2>&1) || true

if [ ! -e "$tmp/test" ]; then
	echo ".PRECIOUS did not preserve the failed output" >&2
	exit 1
fi
