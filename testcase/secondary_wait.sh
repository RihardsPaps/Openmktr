#!/bin/bash

# A .WAIT token produced during secondary expansion must become a graph
# barrier, not merely a text prerequisite.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.SECONDEXPANSION:
waitword = .WAIT

all: first $$(waitword) second
	@test -f first -a -f second
	@printf secondary-wait > all

first:
	@sleep 1
	@printf first > first

second:
	@test -f first
	@printf second > second
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ -f "$tmp/all" ] || exit 1

rm -f "$tmp/first" "$tmp/second" "$tmp/all"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j3 >/dev/null)
[ -f "$tmp/all" ]
