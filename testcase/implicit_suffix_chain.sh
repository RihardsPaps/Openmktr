#!/bin/bash

# Suffix rules must be chainable: GNU make can derive foo.o through
# .c.o after deriving foo.c through .l.c.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.SUFFIXES: .l .c .o

all: sample.o

.l.c:
	@cp $< $@

.c.o:
	@{ printf object:; cat $<; } > $@
EOF

printf 'lexer-source\n' >"$tmp/sample.l"

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/sample.o")" = object:lexer-source ] || exit 1
