#!/bin/sh

# .LOW_RESOLUTION_TIME compares listed prerequisites at whole-second
# precision, while ordinary prerequisites retain normal timestamp behavior.
set -u

mk=$(realpath "$1")

run_case() (
	mode=$1
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	case "$mode" in
	ninja-no-stat|ninja-double-dash)
		mkdir "$tmp/bin"
		cat >"$tmp/bin/stat" <<'STAT'
#!/bin/sh
echo "unexpected external stat invocation" >&2
exit 97
STAT
		chmod +x "$tmp/bin/stat"
		PATH="$tmp/bin:$PATH"
		export PATH
		;;
	esac
	if [ "$mode" = ninja-double-dash ]; then
		cat >"$tmp/Makefile" <<'EOF'
.LOW_RESOLUTION_TIME: --

all: --
	@count=$$(cat count 2>/dev/null || echo 0); echo $$((count + 1)) > count
	@touch $@
EOF
		input_path="$tmp/--"
		printf input >"$input_path"
	else
		cat >"$tmp/Makefile" <<'EOF'
.LOW_RESOLUTION_TIME: input

all: input
	@count=$$(cat count 2>/dev/null || echo 0); echo $$((count + 1)) > count
	@touch $@
EOF
		input_path="$tmp/input"
		printf input >"$input_path"
	fi

	if [ "$mode" = direct ]; then
		(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
	else
		(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
		(cd "$tmp" && ./ninja.sh -j2 all >/dev/null)
	fi

	first=$(cat "$tmp/count")
	sec=$(python -c 'import os,sys; print(int(os.stat(sys.argv[1]).st_mtime))' "$tmp/all")
	python -c 'import os,sys; os.utime(sys.argv[1], (int(sys.argv[2]), int(sys.argv[2])))' "$input_path" "$sec"
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

	python -c 'import os,sys; os.utime(sys.argv[1], (int(sys.argv[2]), int(sys.argv[2])))' "$input_path" "$((sec + 2))"
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
run_case ninja-no-stat
run_case ninja-double-dash
