"""Update only the two verified recovered equals-in-filename snapshots."""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import regression as suite
suite.KATI = Path(os.environ.get("KATI_BINARY", suite.KATI)).resolve()

actual = suite.collect("equal_in_target.mk")
assert len(actual) == 2
for key, result in actual.items():
    assert result["status"] == 0 and result["output"].endswith("PASS\n"), (key, result)
golden = Path(sys.argv[1]) if len(sys.argv) > 1 else suite.GOLDEN
expected = json.loads(golden.read_text())
for key, result in actual.items():
    assert key in expected and expected[key]["status"] == 1
    expected[key] = result
golden.write_text(json.dumps(expected, indent=2, sort_keys=True) + "\n")
print("Updated only two equals-in-filename cases; GNU Make comparison verified PASS")
