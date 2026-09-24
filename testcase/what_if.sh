#!/bin/sh

# GNU make what-if mode makes named normal prerequisites appear newer without
# creating or modifying those files. Order-only prerequisites remain excluded
# from timestamp decisions.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: out

out: dep | order
	@printf out >> log; : > out

dep:
	@printf dep >> log; : > dep

order:
	@printf order >> log; : > order
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
case "$(cat "$tmp/log")" in
  deporderout|orderdepout) ;;
  *) exit 1 ;;
esac

: >"$tmp/log"
(cd "$tmp" && "$mk" -W ./dep -f Makefile all >/dev/null)
[ "$(cat "$tmp/log")" = out ] || {
  echo "-W did not rebuild the dependent target" >&2
  exit 1
}

: >"$tmp/log"
(cd "$tmp" && "$mk" --new-file=dep -f Makefile all >/dev/null)
[ "$(cat "$tmp/log")" = out ] || {
  echo "--new-file did not rebuild the dependent target" >&2
  exit 1
}

: >"$tmp/log"
(cd "$tmp" && "$mk" -W order -f Makefile all >/dev/null)
[ ! -s "$tmp/log" ] || {
  echo "what-if incorrectly affected an order-only prerequisite" >&2
  exit 1
}
