#!/bin/sh

# A backslash-newline in a recipe is one shell command and must survive graph
# generation as one valid recipe in Ninja.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all:
	@printf one \
	  && printf two > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/all")" = two ] || exit 1

rm -f "$tmp/all"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j1 all >/dev/null)
[ "$(cat "$tmp/all")" = two ] || exit 1
