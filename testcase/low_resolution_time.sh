#!/bin/bash

# .LOW_RESOLUTION_TIME compares listed prerequisites at whole-second
# precision, while ordinary prerequisites retain normal timestamp behavior.
set -u

mk=$(realpath "$1")

run_case() {
	local mode=$1
	local tmp
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' RETURN
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

	local first second sec
	first=$(cat "$tmp/count")
	sec=$(stat -c %Y "$tmp/all")
	touch -d "@$sec" "$tmp/input"
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

	touch -d "@$((sec + 2))" "$tmp/input"
	if [ "$mode" = direct ]; then
		(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
	else
		(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
	fi
	if [ "$(cat "$tmp/count")" -le "$second" ]; then
		echo ".LOW_RESOLUTION_TIME ignored a full-second change ($mode)" >&2
		return 1
	fi
}

run_case direct
run_case ninja
