#!/bin/sh
set -eu

kati=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all one two child-one child-two

all: one two

one:
	@$(MAKE) child-one

two:
	@$(MAKE) child-two

child-one child-two:
	@test ! -e active
	@touch active
	@sleep 0.05
	@rm -f active
EOF

(cd "$tmp" && "$kati" -j2 -f Makefile all >/dev/null)
