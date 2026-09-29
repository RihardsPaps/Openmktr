#!/usr/bin/env python3
"""Run a small SQL and persistence smoke test against a built SQLite CLI."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    sqlite = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="ckati-sqlite-") as directory:
        database = Path(directory) / "test.db"
        commands = """
        CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT NOT NULL);
        INSERT INTO items(value) VALUES ('alpha'), ('beta');
        CREATE INDEX items_value ON items(value);
        SELECT group_concat(value, ',') FROM
          (SELECT value FROM items ORDER BY value);
        PRAGMA integrity_check;
        """
        first = subprocess.run(
            [sqlite, str(database)], input=commands, text=True,
            capture_output=True, check=True,
        )
        assert first.stdout.splitlines() == ["alpha,beta", "ok"], first.stdout
        second = subprocess.run(
            [sqlite, str(database), "SELECT count(*) FROM items;"],
            text=True, capture_output=True, check=True,
        )
        assert second.stdout.strip() == "2", second.stdout
        version = subprocess.check_output([sqlite, "--version"], text=True).strip()
        print(f"SQLite {version}; persisted SQL and integrity_check PASS")


if __name__ == "__main__":
    main()
