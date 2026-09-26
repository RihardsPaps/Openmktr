#!/bin/sh

# GNU make reads every -f makefile in order, so definitions in an earlier
# file must be available while later files are parsed.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/first.mk" <<'EOF'
from_first := available
EOF

cat >"$tmp/second.mk" <<'EOF'
ifeq ($(from_first),available)
from_second := available
else
$(error earlier -f makefile was not parsed)
endif

all:
	@test "$(words $(MAKEFILE_LIST))" -eq 2
	@test "$(from_second)" = available
EOF

(cd "$tmp" && "$mk" -f first.mk -f second.mk all)
