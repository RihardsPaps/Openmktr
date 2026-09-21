#!/bin/bash

# Pattern-rule prerequisites must retain .WAIT ordering after the stem is
# substituted. Test direct Kati and the generated Ninja graph.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: sample.out

%.out: %.first .WAIT %.second
	@test -f $*.first -a -f $*.second
	@printf output > $@

sample.first:
	@sleep 1
	@printf first > $@

sample.second:
	@test -f sample.first
	@printf second > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ -f "$tmp/sample.out" ] || exit 1

rm -f "$tmp/sample.first" "$tmp/sample.second" "$tmp/sample.out"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j3 >/dev/null)
[ -f "$tmp/sample.out" ]
