#!/bin/sh

# A multi-step implicit chain must be selected generically and execute in the
# correct prerequisite order in both direct and generated-Ninja builds.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: final.out

%.out: %.mid
	@{ printf out:; cat $<; } > $@

%.mid: %.src
	@{ printf mid:; cat $<; } > $@

%.src:
	@printf source > $@
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/final.out")" = out:mid:source ] || exit 1

rm -f "$tmp/final.out" "$tmp/final.mid" "$tmp/final.src"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/final.out")" = out:mid:source ] || exit 1
