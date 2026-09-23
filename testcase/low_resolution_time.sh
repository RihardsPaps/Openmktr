#!/bin/sh

# .LOW_RESOLUTION_TIME compares listed prerequisites at whole-second
# precision, while ordinary prerequisites retain normal timestamp behavior.
set -u

mk=$(realpath "$1")

run_case() (
	mode=$1
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	cat >"$tmp/Makefile" <<'EOF'
.LOW_RESOLUTION_TIME: input

all: input
	@count=$$(cat count 2>/dev/null || echo 0); echo $$((count + 1)) > count
	@touch $@
EOF
	printf input >"$tmp/input"

	if [ "$mode" = direct ]; then
		(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
	else
		(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
		(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
	fi

	first=$(cat "$tmp/count")
	sec=$(python -c 'import os,sys; print(int(os.stat(sys.argv[1]).st_mtime))' "$tmp/all")
	python -c 'import os,sys; os.utime(sys.argv[1], (int(sys.argv[2]), int(sys.argv[2])))' "$tmp/input" "$sec"
	if [ "$mode" = direct ]; then
		(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
	else
		(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
	fi
	second=$(cat "$tmp/count")
	if [ "$second" != "$first" ]; then
		echo ".LOW_RESOLUTION_TIME rebuilt for a same-second change ($mode)" >&2
		return 1
	fi

	python -c 'import os,sys; os.utime(sys.argv[1], (int(sys.argv[2]), int(sys.argv[2])))' "$tmp/input" "$((sec + 2))"
	if [ "$mode" = direct ]; then
		(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
	else
		(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
	fi
	if [ "$(cat "$tmp/count")" -le "$second" ]; then
		echo ".LOW_RESOLUTION_TIME ignored a full-second change ($mode)" >&2
		return 1
	fi
)

run_case direct
run_case ninja
