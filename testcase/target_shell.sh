#!/bin/bash

# Target-specific SHELL and .SHELLFLAGS must be honored by both direct Kati
# execution and generated Ninja recipes.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: bash-target

bash-target: SHELL := /bin/bash
bash-target: .SHELLFLAGS := -ec
bash-target:
	@[[ "bash" = "bash" ]] && printf success > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/bash-target")" = success ] || exit 1

rm -f "$tmp/bash-target"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j1 all >/dev/null)
[ "$(cat "$tmp/bash-target")" = success ] || exit 1
