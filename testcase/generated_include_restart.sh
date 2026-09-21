#!/bin/sh
set -eu

kati=${1:?path to ckati}
kati=$(cd "$(dirname "$kati")" && pwd)/$(basename "$kati")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
include generated.mk

all: FORCE
	@test "$(compiler_kind)" = clang

FORCE:

generated.mk: compiler.kind
	printf 'compiler_kind := %s\n' "$$(cat $<)" > $@
EOF

printf 'clang\n' >"$tmp/compiler.kind"
printf 'compiler_kind := gcc\n' >"$tmp/generated.mk"
touch -d '1 minute ago' "$tmp/generated.mk"

(cd "$tmp" && "$kati" -f Makefile all)
test "$(cat "$tmp/generated.mk")" = 'compiler_kind := clang'
