#!/bin/sh

# A generic %/ rule must create missing parent directories before a file
# target's recipe runs in direct Kati execution.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: out/result

%/:
	@mkdir -p $(@:%/=%)

out/result: input
	@printf built > $@

input:
	@touch $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/out/result")" = built ]

rm -rf "$tmp/out"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/out/result")" = built ]
