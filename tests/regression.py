#!/usr/bin/env python3
"""Self-contained Kati regression snapshots. No reference make is required."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
KATI = ROOT / "ckati"
CASES = ROOT / "testcase"
GOLDEN = Path(__file__).with_name("golden.json")
TARGET = re.compile(r"^test\d*", re.MULTILINE)
IGNORED_FILES = {"Makefile", "build.ninja", "env.sh", "ninja.sh", "submake"}
# This fixture runs a system-wide socket-listing command with volatile output.
REFERENCE_CASES = {"semicolon_in_var.mk"}
UNORDERED_OUTPUT_CASES = {
    "auto_vars.mk",
    "circular_dep.mk",
    "curdir.mk",
    "curdir_implicit_rule.mk",
    "equal_and_semi_in_rule.mk",
    "ignore.mk",
    "implicit_pattern_rule_for_no_commands.mk",
    "multi_outputs.mk",
    "not_command_with_tab.mk",
    "order_only2.mk",
    "override.mk",
    "preserve_single_dot.mk",
    "semi_in_var.mk",
    "stem.mk",
    "target_specific_var_append.mk",
    "target_specific_var_with_pattern.mk",
}
UNORDERED_SCRIPT_OUTPUT = {"ninja_pool.sh"}
# The historical .PRECIOUS script depends on timestamp granularity in Ninja's
# failed-recipe cleanup and is not a stable snapshot on tmpfs.
UNSTABLE_SCRIPTS = {"precious_delete_on_error.sh"}


def normalize(value: str, directory: Path) -> str:
    value = value.replace(str(directory), "<TESTDIR>").replace(str(KATI), "<KATI>")
    value = re.sub(r"/tmp-test-[^/\s]+", "<TESTDIR-SUBST>", value)
    value = re.sub(r"/tmp/tmp[^/\s]+", "<SCRIPT-TEMP>", value)
    return re.sub(r"(src/[A-Za-z_]+\.cc):\d+:", r"\1:<LINE>:", value)


def invoke(command: list[str], directory: Path) -> tuple[int, str]:
    env = os.environ.copy()
    env.pop("MAKEFLAGS", None)
    env.pop("MAKELEVEL", None)
    env["LC_ALL"] = "C"
    env["TZ"] = "UTC"
    env["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    try:
        result = subprocess.run(
            command,
            cwd=directory,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
            check=False,
        )
        return result.returncode, normalize(result.stdout.decode("utf-8", "replace"), directory)
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode("utf-8", "replace")
        return 124, normalize(output + "<TIMEOUT>\n", directory)


def created_files(directory: Path) -> list[str]:
    return sorted(
        child.name
        for child in directory.iterdir()
        if child.name not in IGNORED_FILES
        and not child.name.startswith(".kati")
        and not child.name.startswith("stdout")
    )


def run_makefile(source: Path, target: str, ninja: bool) -> dict:
    with tempfile.TemporaryDirectory(prefix="kati-test-") as temp:
        directory = Path(temp)
        (directory / "Makefile").write_bytes(source.read_bytes())
        (directory / "submake").symlink_to(CASES / "submake", target_is_directory=True)
        command = [str(KATI), "--use_find_emulator", "-j1"]
        if ninja:
            command.append("--ninja")
        command.extend(["SHELL=/bin/sh"])
        if target:
            command.append(target)
        status, output = invoke(command, directory)
        if ninja and status == 0 and (directory / "ninja.sh").exists():
            ninja_status, ninja_output = invoke(["sh", "./ninja.sh", "-j1"], directory)
            status = ninja_status
            output += ninja_output
        if not ninja and source.name in UNORDERED_OUTPUT_CASES:
            output = "".join(sorted(output.splitlines(keepends=True)))
        if not ninja and source.name == "auto_var_suffixes.mk" and target == "test2":
            # Independent missing prerequisites can be diagnosed in either order.
            output = ""
        return {"status": status, "output": output, "files": created_files(directory)}


def run_script(source: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="kati-script-") as temp:
        directory = Path(temp)
        command = ["sh", str(source), str(KATI)]
        if source.name.startswith("ninja_"):
            command.extend(["--ninja", "--regen"])
        command.append("SHELL=/bin/sh")
        status, output = invoke(command, directory)
        if source.name in UNORDERED_SCRIPT_OUTPUT:
            output = re.sub(r"\[\d+/4\] build", "[STEP] build", output)
            output = "".join(sorted(output.splitlines(keepends=True)))
        return {"status": status, "output": output, "files": created_files(directory)}


def run_parallel() -> dict:
    with tempfile.TemporaryDirectory(prefix="kati-parallel-") as temp:
        directory = Path(temp)
        (directory / "Makefile").write_text(
            "all: done-0 done-1 done-2 done-3\n"
            + "".join(f"done-{index}:\n\t@python worker.py {index}\n" for index in range(4)),
            encoding="utf-8",
        )
        (directory / "worker.py").write_text(
            "from pathlib import Path\n"
            "import sys, time\n"
            "Path('start-' + sys.argv[1]).touch()\n"
            "deadline = time.monotonic() + 3\n"
            "while len(list(Path('.').glob('start-*'))) < 4:\n"
            "    if time.monotonic() > deadline: raise SystemExit(1)\n"
            "    time.sleep(0.01)\n"
            "Path('done-' + sys.argv[1]).touch()\n",
            encoding="utf-8",
        )
        status, output = invoke([str(KATI), "-j4", "-s", "all"], directory)
        return {
            "status": status,
            "completed": len(list(directory.glob("done-*"))),
            "output": output if status else "",
        }


def collect() -> dict:
    snapshots = {}
    for source in sorted(CASES.glob("*.mk")):
        if source.name in REFERENCE_CASES:
            continue
        targets = sorted(set(TARGET.findall(source.read_text(encoding="utf-8", errors="replace"))))
        for target in targets or [""]:
            for ninja in (False, True):
                key = f"{source.name}:{target or 'default'}:{'ninja' if ninja else 'direct'}"
                snapshots[key] = run_makefile(source, target, ninja)
    for source in sorted(CASES.glob("*.sh")):
        if source.name in UNSTABLE_SCRIPTS:
            continue
        snapshots[f"{source.name}:script"] = run_script(source)
    snapshots["parallel:direct"] = run_parallel()
    return snapshots


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", action="store_true", help="regenerate checked-in snapshots")
    args = parser.parse_args()
    if not KATI.is_file():
        parser.error("build ckati first")
    actual = collect()
    if args.record:
        GOLDEN.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Recorded {len(actual)} scenarios in {GOLDEN}")
        return 0
    expected = json.loads(GOLDEN.read_text(encoding="utf-8"))
    failures = 0
    for key in sorted(set(actual) | set(expected)):
        if actual.get(key) != expected.get(key):
            failures += 1
            print(f"FAIL {key}")
            print("expected:", json.dumps(expected.get(key), ensure_ascii=False))
            print("actual:  ", json.dumps(actual.get(key), ensure_ascii=False))
    print(f"{len(actual) - failures}/{len(actual)} scenarios match")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
