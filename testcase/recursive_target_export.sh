#!/bin/bash

# Target-specific exported variables must reach a recursive child in both
# direct execution and generated-Ninja execution.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/child"

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all recurse

all: recurse

recurse: export CHILD_VALUE = from-target
recurse:
	@$(MAKE) -C child all
EOF

cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all
all:
	@printf '%s' "$CHILD_VALUE" > child.out
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/child/child.out")" = from-target ]

rm -f "$tmp/child/child.out"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/child/child.out")" = from-target ]
