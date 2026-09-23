#!/bin/sh

# Final cross-feature conformance case: VPATH resolution, secondary
# expansion, a .WAIT barrier, and target-specific shell selection must remain
# correct together in direct and generated-Ninja execution.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/src" "$tmp/build"
printf '%s' input >"$tmp/src/input.txt"

cat >"$tmp/Makefile" <<'EOF'
.SECONDEXPANSION:
VPATH = src
SOURCE_NAME = input.txt

.PHONY: all
all: build/result

build/result: SHELL := /bin/sh
build/result: .SHELLFLAGS := -ec
build/result: $$(SOURCE_NAME) .WAIT generated
	[ -f "$<" ] && [ -f generated ] && cat "$<" > "$@"

generated:
	@sleep 1; : > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/build/result")" = input ]

rm -f "$tmp/build/result" "$tmp/generated"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j3 all >/dev/null)
[ "$(cat "$tmp/build/result")" = input ]
