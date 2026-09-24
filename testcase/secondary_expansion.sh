#!/bin/sh

# Secondary expansion must resolve escaped prerequisites at graph-build time,
# including when the graph is emitted for Ninja.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.SECONDEXPANSION:
SECOND = second
all: first $$(SECOND)

first:
	@printf first > $@

second:
	@printf second > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ -f "$tmp/first" ] && [ -f "$tmp/second" ] || exit 1

rm -f "$tmp/first" "$tmp/second"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ -f "$tmp/first" ] && [ -f "$tmp/second" ] || exit 1
