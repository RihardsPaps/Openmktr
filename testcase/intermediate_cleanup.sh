#!/bin/sh

# Exercise explicit .INTERMEDIATE cleanup and .SECONDARY protection in both
# direct Kati execution and generated Ninja graphs.
set -u

mk=$(realpath "$1")

run_case() (
	mode=$1
	secondary=$2
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	cat >"$tmp/Makefile" <<EOF
.INTERMEDIATE: temp
$secondary

all: final

temp:
	@printf temporary > \$@

final: temp
	@cat \$< > \$@
EOF

	if [ "$mode" = direct ]; then
		(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
	else
		(cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
		(cd "$tmp" && ./ninja.sh -j2 >/dev/null)
	fi

	[ -f "$tmp/final" ] || {
		echo "final output missing ($mode)" >&2
		return 1
	}
	if [ -n "$secondary" ]; then
		[ -f "$tmp/temp" ] || {
			echo ".SECONDARY did not preserve intermediate output ($mode)" >&2
			return 1
		}
	else
		[ ! -e "$tmp/temp" ] || {
			echo ".INTERMEDIATE output was not cleaned ($mode)" >&2
			return 1
		}
	fi
)

run_case direct ''
run_case direct '.SECONDARY: temp'
run_case direct '.SECONDARY:'
run_case ninja ''
run_case ninja '.SECONDARY: temp'
run_case ninja '.SECONDARY:'
