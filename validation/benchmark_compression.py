#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compare real workspace pack/verified-restore costs using source and objects."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import statistics
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("workspace", args.repository / "tools/omktr_workspace.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    samples = []
    with tempfile.TemporaryDirectory(prefix="omktr-compression-") as temp:
        root = Path(temp)
        seed = root / "seed"
        seed.mkdir()
        for name in ("src", "tests", "out", "omktr"):
            source = args.repository / name
            if source.is_dir():
                shutil.copytree(source, seed / name, ignore=shutil.ignore_patterns("__pycache__"))
            else:
                shutil.copy2(source, seed / name)
        inventory = {str(p.relative_to(seed)): helper.digest(p)
                     for p in seed.rglob("*") if p.is_file()}
        for sample in range(6):
            for compression in (("xz", "zstd") if sample % 2 == 0 else ("zstd", "xz")):
                workspace = root / f"s{sample}-{compression}"
                content = workspace / "content/source"
                shutil.copytree(seed, content)
                state = dict(schema=1, packed=False, last_used=time.time())
                helper.atomic_json(workspace / "state.json", state)
                start = time.perf_counter()
                packed = helper.pack(workspace, state, compression)
                pack_seconds = time.perf_counter() - start
                start = time.perf_counter()
                helper.unpack(workspace, state)
                restore_seconds = time.perf_counter() - start
                if inventory != {str(p.relative_to(content)): helper.digest(p)
                                 for p in content.rglob("*") if p.is_file()}:
                    raise RuntimeError("Restored workspace differs")
                samples.append(dict(sample=sample, warmup=sample == 0, compression=compression,
                                    pack_seconds=pack_seconds, restore_seconds=restore_seconds, **{
                                        k: packed[k] for k in ("original_bytes", "compressed_bytes")}))
                shutil.rmtree(workspace)
    medians = {format: {key: statistics.median(row[key] for row in samples
                                              if row["compression"] == format and not row["warmup"])
                        for key in ("pack_seconds", "restore_seconds", "compressed_bytes")}
               for format in ("xz", "zstd")}
    qualifies = (medians["zstd"]["compressed_bytes"] <= medians["xz"]["compressed_bytes"] * 1.25
                 and all(medians["zstd"][key] < medians["xz"][key]
                         for key in ("pack_seconds", "restore_seconds")))
    report = dict(protocol="one warmup; five alternating measured samples; verified file hashes",
                  content="repository C++ sources, tests, LLVM objects and executable",
                  python=sys.version, file_count=len(inventory),
                  input_manifest_sha256=hashlib.sha256(json.dumps(inventory, sort_keys=True).encode()).hexdigest(),
                  harness_sha256=helper.digest(Path(__file__)),
                  executable_sha256=helper.digest(args.repository / "omktr"),
                  settings=dict(xz_preset=6, zstd_level=3), samples=samples, medians=medians,
                  selected_default="zstd" if qualifies else "xz")
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(medians=medians, selected_default=report["selected_default"])))


if __name__ == "__main__":
    main()
