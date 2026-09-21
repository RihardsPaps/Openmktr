#!/bin/bash
set -eu

kati=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
LIB = libfoo.a
BUILD_LIBS = $(LIB)
BUILD_LIBS += -global

all: gen

gen: BUILD_LIBS += -lm
gen:
	@printf '%s\n' "$(BUILD_LIBS)" > result
EOF

(cd "$tmp" && "$kati" -f Makefile all >/dev/null)
[ "$(cat "$tmp/result")" = "libfoo.a -global -lm" ]

rm -f "$tmp/result"
(cd "$tmp" && "$kati" --ninja --regen -f Makefile all >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/result")" = "libfoo.a -global -lm" ]
