#!/bin/sh

# GNU make touch mode touches stale or missing outputs without recipes.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: phony
all: phony output

phony:
	@printf ran > phony.ran

output: input
	@printf original > output
EOF

touch "$tmp/input"
(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/output")" = original ] || exit 1
rm -f "$tmp/phony.ran"
before=$(python -c 'import os,sys; print(int(os.stat(sys.argv[1]).st_mtime))' "$tmp/output")
sleep 1
touch "$tmp/input"
(cd "$tmp" && "$mk" -t -f Makefile all >/dev/null)
after=$(python -c 'import os,sys; print(int(os.stat(sys.argv[1]).st_mtime))' "$tmp/output")
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
  echo "touch mode did not create a missing output" >&2
  exit 1
}
[ ! -s "$tmp/output" ] || {
  echo "touch mode ran the missing output's recipe" >&2
  exit 1
}
