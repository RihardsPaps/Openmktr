#!/bin/bash

# GNU make touch mode updates existing outputs without executing recipes, but
# still builds a missing output.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: phony
all: phony output

phony:
	@printf ran > phony.ran

output:
	@printf original > output
EOF

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/output")" = original ] || exit 1
rm -f "$tmp/phony.ran"
before=$(stat -c %Y "$tmp/output")
sleep 1
(cd "$tmp" && "$mk" -t -f Makefile all >/dev/null)
after=$(stat -c %Y "$tmp/output")
[ "$after" -gt "$before" ] || {
  echo "touch mode did not update an existing output" >&2
  exit 1
}
[ "$(cat "$tmp/output")" = original ] || {
  echo "touch mode ran the recipe" >&2
  exit 1
}
[ ! -e "$tmp/phony.ran" ] || {
  echo "touch mode ran a phony recipe" >&2
  exit 1
}

rm -f "$tmp/output"
(cd "$tmp" && "$mk" --touch -f Makefile all >/dev/null)
[ -f "$tmp/output" ] || {
  echo "touch mode did not build a missing output" >&2
  exit 1
}
