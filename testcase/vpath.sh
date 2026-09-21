#!/bin/bash

# Both global VPATH and pattern-specific vpath must resolve source
# prerequisites without changing the logical target names.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/src" "$tmp/alt"
printf global >"$tmp/src/input"
printf pattern >"$tmp/alt/pattern.dat"

cat >"$tmp/Makefile" <<'EOF'
VPATH = src
vpath %.dat alt

all: copied pattern.out

copied: input
	@cat $< > $@

pattern.out: pattern.dat
	@cat $< > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/copied")" = global ] || exit 1
[ "$(cat "$tmp/pattern.out")" = pattern ] || exit 1

rm -f "$tmp/copied" "$tmp/pattern.out"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/copied")" = global ] || exit 1
[ "$(cat "$tmp/pattern.out")" = pattern ] || exit 1
