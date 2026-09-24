#!/bin/sh
set -eu

kati=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all:
	@printf '%s\n' "$(CC)" > result
EOF

(cd "$tmp" && "$kati" -f Makefile CC=first CC=second all >/dev/null)
[ "$(cat "$tmp/result")" = "second" ]

(cd "$tmp" && "$kati" -f Makefile CC=second CC=first all >/dev/null)
[ "$(cat "$tmp/result")" = "first" ]
