#!/bin/sh
# Kbuild adds its source directory to MAKEFLAGS so recursive makes can load
# relative include files while building in a separate output directory.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fragments" "$tmp/output"
cp "$mk" "$tmp/omktr"
mk="$tmp/omktr"

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

# A command-line include directory must survive the parent/child boundary,
# including the form with an escaped space in MAKEFLAGS.
cat >"$tmp/ParentOptions.mk" <<'EOF'
.PHONY: all
all:
	@$(MAKE) -C output -f $(CURDIR)/Child.mk result
EOF
rm "$tmp/output/result"
(cd "$tmp" && "$mk" -I "$tmp" -f ParentOptions.mk all)
[ "$(cat "$tmp/output/result")" = configured ]
rm "$tmp/output/result"
(cd "$tmp" && "$mk" --include-dir="$tmp" -f ParentOptions.mk all)
[ "$(cat "$tmp/output/result")" = configured ]
rm "$tmp/output/result"
(cd "$tmp" && "$mk" --ninja -I "$tmp" -f ParentOptions.mk all &&
 sh ninja.sh -j2 >/dev/null)
[ "$(cat "$tmp/output/result")" = configured ]
printf 'recursive options PASS\n'

mkdir -p "$tmp/include dir"
cat >"$tmp/SpaceChild.mk" <<'EOF'
include spaced.mk
EOF
cat >"$tmp/include dir/spaced.mk" <<'EOF'
spaced:
	@printf 'spaced\n' > $@
EOF
cat >"$tmp/SpaceParent.mk" <<'EOF'
.PHONY: all
all:
	@$(MAKE) -C output -f $(CURDIR)/SpaceChild.mk spaced
EOF
(cd "$tmp" && "$mk" -I "$tmp/include dir" -f SpaceParent.mk all)
[ "$(cat "$tmp/output/spaced")" = spaced ]
printf 'spaced option PASS\n'

# GNU make searches -I directories for literal names only. A wildcard with
# no local matches must not load a matching file from an include directory.
mkdir -p "$tmp/wildcards"
cat >"$tmp/wildcards/unexpected.mk" <<'EOF'
$(error included a wildcard from the search directory)
EOF
cat >"$tmp/Wildcard.mk" <<'EOF'
-include *.mk
.PHONY: all
all:
	@printf 'wildcard-safe\n' > wildcard.out
EOF
(cd "$tmp/output" && "$mk" -I "$tmp/wildcards" -f "$tmp/Wildcard.mk" all)
[ "$(cat "$tmp/output/wildcard.out")" = wildcard-safe ]
printf 'wildcard PASS\n'
