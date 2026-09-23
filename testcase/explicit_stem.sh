#!/bin/sh

# GNU make supplies $* for an explicit target with a recognized suffix.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: sample.h

sample.h:
	@printf '%s\n' '$*' > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/sample.h")" = sample ]
