"""Audit recovered crashes; retain all unaffected regression expectations."""
import json
import sys

sys.path.insert(0, "/workspace/tests")
import regression as suite

actual = suite.collect()
known = set(json.loads(suite.KNOWN_CRASHES.read_text()))
expected = json.loads(suite.GOLDEN.read_text())
recovered = sorted(key for key in known if not suite.is_crash(actual[key]))
unexpected = [key for key in actual if key not in known
              and actual[key] != expected.get(key)]
if unexpected:
    raise SystemExit("Unexpected changes: " + ", ".join(unexpected))
for key in recovered:
    print(key, json.dumps(actual[key]))
    expected[key] = actual[key]
suite.KNOWN_CRASHES.write_text(json.dumps(sorted(known - set(recovered)), indent=2) + "\n")
suite.GOLDEN.write_text(json.dumps(expected, indent=2, sort_keys=True) + "\n")
print(f"Recovered {len(recovered)} cases; {len(known) - len(recovered)} remain")
