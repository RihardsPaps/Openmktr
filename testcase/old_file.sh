#!/bin/bash

# GNU make old-file mode suppresses the named target's recipe while retaining
# its real timestamp for dependent comparisons.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: out

out: dep
	@printf out >> log; : > out

dep:
	@printf dep >> log; : > dep
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/log")" = depout ] || exit 1

# Make dep newer than out. -o must skip dep's recipe but allow out to see the
# real newer timestamp and rebuild.
sleep 1
printf changed >"$tmp/dep"
: >"$tmp/log"
(cd "$tmp" && "$mk" -o dep -f Makefile all >/dev/null)
[ "$(cat "$tmp/log")" = out ] || {
  echo "-o rebuilt or failed to preserve the old target" >&2
  exit 1
}

: >"$tmp/log"
(cd "$tmp" && "$mk" --old-file=dep -f Makefile all >/dev/null)
[ ! -s "$tmp/log" ] || {
  echo "--old-file did not suppress the named target's dependency rebuild" >&2
  exit 1
}
