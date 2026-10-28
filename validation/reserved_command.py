#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Run one external diagnostic/build against the existing durable budget."""
import argparse
import json
import os
from pathlib import Path

from benchmark_ninja import digest, run, source_manifest
from qualify_dependencies import Budget, check_timestamp_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ledger", type=Path, required=True)
    parser.add_argument("--command-limit", type=int, default=200,
                        help="explicit user-authorized total cap; never resets the ledger")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cwd", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--environment", type=Path, required=True)
    parser.add_argument("--purpose", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required")
    env = json.loads(args.environment.read_text())
    check_timestamp_environment(env)
    args.output.mkdir(parents=True, exist_ok=False)
    before = source_manifest(args.source)
    budget = Budget(args.ledger, limit=args.command_limit)
    record = dict(command=command, environment=env, harness_sha256=digest(Path(__file__)),
                  command_limit=budget.limit)
    try:
        record["reservation"] = budget.reserve(command, args.cwd, args.purpose)
        print(json.dumps(record["reservation"]), flush=True)
        record["phase"] = run(command, args.cwd, args.output / "command.log", env,
                              args.output / "commands.jsonl")
    except BaseException as error:
        record["error"] = str(error)
        raise
    finally:
        record["source_changes"] = source_manifest(args.source) != before
        record["consumed_commands"] = budget.used
        (args.output / "report.json").write_text(json.dumps(record, indent=2) + "\n")
        budget.close()
        if record["source_changes"]:
            raise RuntimeError("Release source contents changed")


if __name__ == "__main__":
    main()
