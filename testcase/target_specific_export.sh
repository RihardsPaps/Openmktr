#!/bin/bash
set -eu

kati=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all recurse other

VALUE = global

all: recurse other

recurse: private _VALUE := $(VALUE)-target
recurse: export VALUE = $(_VALUE)
recurse:
	@$(MAKE) -f child.mk all

other:
	@printf '%s\n' "$${VALUE-unset}" > other.result
EOF

cat >"$tmp/child.mk" <<'EOF'
.PHONY: all
all:
	@printf '%s\n' "$${VALUE-unset}" > child.result
EOF

(cd "$tmp" && "$kati" -f Makefile all >/dev/null)
[ "$(cat "$tmp/child.result")" = "global-target" ]
[ "$(cat "$tmp/other.result")" = "unset" ]

rm -f "$tmp/child.result" "$tmp/other.result"
(cd "$tmp" && "$kati" --ninja --regen -f Makefile all >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/child.result")" = "global-target" ]
[ "$(cat "$tmp/other.result")" = "unset" ]
