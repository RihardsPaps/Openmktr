#!/bin/sh
# Kbuild adds its source directory to MAKEFLAGS so recursive makes can load
# relative include files while building in a separate output directory.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fragments" "$tmp/output"
cp "$mk" "$tmp/ckati"
mk="$tmp/ckati"

cat >"$tmp/Makefile" <<'EOF'
MAKEFLAGS += --include-dir=$(CURDIR)

.PHONY: all
all:
	@$(MAKE) -C output -f $(CURDIR)/Child.mk result
EOF

cat >"$tmp/Child.mk" <<'EOF'
include fragments/rules.mk
EOF

cat >"$tmp/fragments/rules.mk" <<'EOF'
result:
	@printf 'configured\n' > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all)
[ "$(cat "$tmp/output/result")" = configured ]
printf 'direct PASS\n'

rm "$tmp/output/result"
(cd "$tmp" && "$mk" --ninja -f Makefile all && sh ninja.sh -j2 >/dev/null)
[ "$(cat "$tmp/output/result")" = configured ]
printf 'ninja PASS\n'

rm "$tmp/output/result"
(cd "$tmp/output" && "$mk" -I "$tmp" -f "$tmp/Child.mk" result)
[ "$(cat "$tmp/output/result")" = configured ]
rm "$tmp/output/result"
(cd "$tmp/output" && "$mk" --include-dir="$tmp" -f "$tmp/Child.mk" result)
[ "$(cat "$tmp/output/result")" = configured ]
printf 'options PASS\n'
