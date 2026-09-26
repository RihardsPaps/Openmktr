#!/bin/sh

set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: test
test:
	printf 'recipe-output\n'
EOF

result=$(cd "$tmp" && "$mk" --quiet -f Makefile test)
[ "$result" = "recipe-output" ]
