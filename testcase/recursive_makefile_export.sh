#!/bin/sh

# Exports assigned by the makefile must reach recursive children in a
# generated Ninja graph.  The graph's env.sh is only a process-environment
# snapshot and must not replace the makefile's effective export value.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/child"

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all recurse

CHILD_VALUE := from-makefile
export CHILD_VALUE

all: recurse
recurse:
	@$(MAKE) -C child all
EOF

cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all
all:
	@printf '%s' "$$CHILD_VALUE" > child.out
EOF

(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/child/child.out")" = from-makefile ]
