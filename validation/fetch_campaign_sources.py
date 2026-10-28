#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Download selected official release archives and verify the campaign lock.

--record records locally observed hashes when establishing a new release lock;
it does not claim independent signature verification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("dependency-campaign.json"))
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--component", action="append")
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    args.archives.mkdir(parents=True, exist_ok=True)
    selected = [p for p in manifest["components"] if not args.component or p["id"] in args.component]
    if args.component and set(args.component) - {p["id"] for p in selected}:
        parser.error("unknown component")
    for release in selected:
        if not release.get("archive"):
            continue
        if not release.get("sha256") and not args.record:
            raise RuntimeError("Unresolved archive hash: " + release["id"])
        name = release["archive"]
        if Path(name).name != name:
            raise RuntimeError("Archive name must be one filename")
        archive = args.archives / name
        if not archive.exists():
            print("Downloading " + release["source"], flush=True)
            fd, temporary = tempfile.mkstemp(prefix=".download-", dir=args.archives)
            try:
                with os.fdopen(fd, "wb") as output, urllib.request.urlopen(release["source"], timeout=60) as incoming:
                    shutil.copyfileobj(incoming, output)
                with tarfile.open(temporary, "r|*") as incoming:
                    if incoming.next() is None:
                        raise RuntimeError("Empty source archive: " + name)
                observed = sha256(Path(temporary))
                if release.get("sha256") and observed != release["sha256"]:
                    raise RuntimeError("Archive checksum mismatch: " + name)
                os.replace(temporary, archive)
            finally:
                Path(temporary).unlink(missing_ok=True)
        observed = sha256(archive)
        if release.get("sha256") and observed != release["sha256"]:
            raise RuntimeError("Archive checksum mismatch: " + name)
        if args.record:
            if not release.get("sha256"):
                release["hash_provenance"] = "Locally observed SHA-256 of official archive; no signature verification claimed"
            release["sha256"] = observed
            release["archive_verified"] = True
            args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
        print(name + " sha256:" + observed, flush=True)


if __name__ == "__main__":
    main()
