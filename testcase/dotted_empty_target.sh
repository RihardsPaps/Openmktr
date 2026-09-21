#!/bin/bash
set -eu

kati=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: result

.state.d: ;

result: .state.d
	@printf '%s\n' ok > $@
EOF

(cd "$tmp" && "$kati" --ninja --regen -f Makefile all >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/result")" = ok ]
