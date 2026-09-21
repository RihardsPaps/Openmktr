#!/bin/bash

# Test the dependency-free pure subset of $(guile ...), both as a direct
# makefile expansion and in a generated Ninja graph.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
MESSAGE := $(guile (begin (quote ignored) (string-append "hello" "-" (if #t "world" "bad"))))
.PHONY: all
all:
	@printf '%s' "$(MESSAGE)" > result
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/result")" = hello-world ]

rm -f "$tmp/result"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
[ "$(cat "$tmp/result")" = hello-world ]

cat >"$tmp/Makefile" <<'EOF'
all:
	@true
$(guile (system "touch should-not-exist"))
EOF
if (cd "$tmp" && "$mk" -f Makefile all >out 2>&1); then
  exit 1
fi
grep -F 'guile: procedure `system' "$tmp/out" >/dev/null
[ ! -e "$tmp/should-not-exist" ]
