#!/bin/sh

# Both global VPATH and pattern-specific vpath must resolve source
# prerequisites without changing the logical target names.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/src" "$tmp/alt"
mkdir -p "$tmp/src/include"
printf global >"$tmp/src/input"
printf pattern >"$tmp/alt/pattern.dat"
printf '#include "version.h"\n' >"$tmp/src/include/api.h"
printf '#define VPATH_HEADER_VALUE 42\n' >"$tmp/src/include/version.h"
printf '#include <include/api.h>\nVPATH_HEADER_VALUE\n' >"$tmp/main.c"

cat >"$tmp/Makefile" <<'EOF'
VPATH = src
vpath %.dat alt

all: copied pattern.out copied-header preprocessed

copied: input
	@cat $< > $@

pattern.out: pattern.dat
	@cat $< > $@

# The no-op rule makes the VPATH header an explicit target. Ninja must not
# materialize it in the build tree, where it would shadow sibling includes.
%.h: ; @:

copied-header: include/api.h
	@cp $< $@

preprocessed: include/api.h main.c
	@clang -I. -Isrc -E main.c -o $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/copied")" = global ] || exit 1
[ "$(cat "$tmp/pattern.out")" = pattern ] || exit 1
[ "$(cat "$tmp/copied-header")" = '#include "version.h"' ] || exit 1
grep -q '42' "$tmp/preprocessed" || exit 1
[ ! -e "$tmp/include/api.h" ] || exit 1

rm -f "$tmp/copied" "$tmp/pattern.out" "$tmp/copied-header" \
  "$tmp/preprocessed"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/copied")" = global ] || exit 1
[ "$(cat "$tmp/pattern.out")" = pattern ] || exit 1
[ "$(cat "$tmp/copied-header")" = '#include "version.h"' ] || exit 1
grep -q '42' "$tmp/preprocessed" || exit 1
[ ! -e "$tmp/include/api.h" ] || exit 1
