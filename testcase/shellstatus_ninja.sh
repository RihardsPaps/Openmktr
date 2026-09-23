#!/bin/sh

# Recipe-level $(shell ...) and .SHELLSTATUS must remain usable when Kati
# emits a Ninja graph. The shell function is evaluated once during graph
# generation and its status is expanded into the generated recipe.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: success failure

success:
	@printf '%s\n' "$(shell printf success)$(.SHELLSTATUS)" > $@

failure:
	@printf '%s\n' "$(shell false)$(.SHELLSTATUS)" > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/success")" = success0 ] || exit 1
[ "$(cat "$tmp/failure")" = 1 ] || exit 1

rm -f "$tmp/success" "$tmp/failure"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 >/dev/null)
[ "$(cat "$tmp/success")" = success0 ] || exit 1
[ "$(cat "$tmp/failure")" = 1 ]
