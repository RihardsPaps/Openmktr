#!/bin/sh -eux

TMPDIR="$(mktemp -d)"
trap 'rm -fr "$TMPDIR"' EXIT
JSON="$TMPDIR/include.json"

cd "$(dirname "$0")"

if ! "$KATI" -f include_smoke.mk nop --dump_include_graph "$JSON"; then
  exit 1
fi

python - "$JSON" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    graph = json.load(stream)["include_graph"]
assert len(graph) == 3
assert sorted(item["file"] for item in graph) == [
    "bottom.mk", "include_smoke.mk", "middle.mk"
]
top = next(item for item in graph if item["file"] == "include_smoke.mk")
assert sorted(top["includes"]) == ["bottom.mk", "middle.mk"]
PY

echo "OK"
