#!/bin/bash

# Recursive make must retain the caller's exported variables and working
# directory in both direct and generated-Ninja execution.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/child"

cat >"$tmp/Makefile" <<EOF
.PHONY: all recurse
export CHILD_VALUE = from-parent

all: recurse

recurse:
	@\$(MAKE) -C child all
EOF

cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all
all:
	@printf '%s' "$CHILD_VALUE" > child.out
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/child/child.out")" = from-parent ] || exit 1

rm -f "$tmp/child/child.out"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/child/child.out")" = from-parent ] || exit 1
