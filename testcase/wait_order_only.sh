#!/bin/bash

# Verify that .WAIT also orders order-only prerequisites in both execution
# modes, without changing the target's automatic prerequisite variables.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: first .WAIT second | gate
	@test "$(firstword $^)" = first
	@test "$(words $^)" -eq 2
	@test -f gate
	@printf 'wait-order-only=ok\n'

first:
	@sleep 1
	@printf first > $@

gate:
	@test -f first
	@printf gate > $@

second:
	@test -f first
	@printf second > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ -f "$tmp/second" ] || exit 1

rm -f "$tmp/first" "$tmp/gate" "$tmp/second"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j3 >/dev/null)
[ -f "$tmp/second" ]
