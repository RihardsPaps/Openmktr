#!/usr/bin/env python3
"""Run a real build phase and append its command/result to the live record."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import shlex
import subprocess
import time


def run(name, command, cwd, report=Path("/report"), env=None):
    report.mkdir(parents=True, exist_ok=True)
    logs = report / "logs"
    logs.mkdir(exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    log = logs / f"{stamp}-{name}.log"
    record = report / "PROGRESS.md"
    with record.open("a", encoding="utf-8") as stream:
        stream.write(f"\n- {stamp} START {name}; cwd `{cwd}`; "
                     f"command `{shlex.join(command)}`; log `{log.name}`.\n")
    start = time.monotonic()
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT, check=False)
    elapsed = time.monotonic() - start
    with record.open("a", encoding="utf-8") as stream:
        stream.write(f"- {name}: {'PASS' if result.returncode == 0 else 'FAIL'} "
                     f"(phase only), exit {result.returncode}, {elapsed:.1f}s.\n")
    print(f"{name}: exit {result.returncode}, {elapsed:.1f}s", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--cwd", required=True)
    parser.add_argument("--report", type=Path, default=Path("/report"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required")
    run(args.name, command, args.cwd, args.report)
