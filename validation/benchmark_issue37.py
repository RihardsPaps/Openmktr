#!/usr/bin/env python3
"""External Linux build benchmark for issue 37; GNU Make is a comparison tool.

Use unmodified FFmpeg 7.1.1 and Linux 6.12 release archives. Builds and logs
are retained under --root; phase and complete clean-build times are reported.
This campaign helper does not form part of the GNU-free portable test gate.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import time


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def manifest(root):
    return {str(p.relative_to(root)): digest(p)
            for p in sorted(root.rglob("*")) if p.is_file() and not p.is_symlink()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--omktr", type=Path, required=True)
    parser.add_argument("--project", choices=("ffmpeg", "linux"), required=True)
    parser.add_argument("--jobs", type=int, nargs="+", default=[8, 16])
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--modes", nargs="+", choices=("make", "ninja"),
                        default=["make", "ninja"])
    parser.add_argument("--label", default="matched")
    parser.add_argument("--trace-exec", action="store_true",
                        help="Diagnostic exec trace; do not compare traced timings")
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    tool = args.omktr.resolve()
    version = "ffmpeg-7.1.1" if args.project == "ffmpeg" else "linux-6.12"
    archive = args.archives.resolve() / (version + ".tar.xz")
    sources = root / "sources"
    source = sources / version
    sources.mkdir(exist_ok=True)
    if not source.exists():
        with tarfile.open(archive) as release:
            release.extractall(sources, filter="data")
    before = manifest(source)
    env = dict(HOME=str(Path.home()), USER="benchmark", LOGNAME="benchmark",
               SHELL="/bin/sh")
    env.update(PATH="/usr/bin:/bin", LC_ALL="C", TZ="UTC", CCACHE_DISABLE="1",
               SOURCE_DATE_EPOCH="1731888000", KBUILD_BUILD_USER="benchmark",
               KBUILD_BUILD_HOST="benchmark", KBUILD_BUILD_VERSION="1",
               KBUILD_BUILD_TIMESTAMP="Mon Nov 18 00:00:00 UTC 2024")
    metadata = {
        "project": version, "archive_sha256": digest(archive),
        "omktr_sha256": digest(tool), "environment": env,
        "versions": {name: subprocess.check_output(command, env=env, text=True)
                     for name, command in {
                         "omktr": [str(tool), "--version"],
                         "make": ["make", "--version"],
                         "clang": ["clang-18", "--version"],
                         "ninja": ["ninja", "--version"],
                         "kernel": ["uname", "-a"],
                     }.items()},
    }
    # Do not persist unrelated inherited environment values in the report.
    metadata["environment"] = {k: env[k] for k in (
        "PATH", "LC_ALL", "TZ", "CCACHE_DISABLE", "SOURCE_DATE_EPOCH",
        "KBUILD_BUILD_USER", "KBUILD_BUILD_HOST", "KBUILD_BUILD_VERSION",
        "KBUILD_BUILD_TIMESTAMP")}
    (root / f"{args.label}-{args.project}-metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n")

    def run(cwd, log, command):
        start = time.monotonic()
        with log.open("wb") as output:
            child = subprocess.Popen(command, cwd=cwd, env=env, stdout=output,
                                     stderr=subprocess.STDOUT)
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
        result = dict(command=command, cwd=str(cwd), log=str(log),
                      status=child.returncode, wall_seconds=time.monotonic() - start,
                      user_seconds=usage.ru_utime, system_seconds=usage.ru_stime,
                      peak_rss_kib=usage.ru_maxrss)
        print(json.dumps(result), flush=True)
        with (root / "commands.jsonl").open("a") as output:
            output.write(json.dumps(result) + "\n")
        if child.returncode:
            raise RuntimeError(f"Command failed; inspect {log}")
        return result

    for jobs in args.jobs:
        for repeat in range(args.repeats):
            # Alternate order to reduce systematic cache/thermal ordering bias.
            modes = args.modes if repeat % 2 == 0 else list(reversed(args.modes))
            for mode in modes:
                name = f"{args.label}-{args.project}-j{jobs}-r{repeat + 1}-{mode}"
                build = root / name
                build.mkdir()  # Never overwrite or clean another run's tree.
                common = []
                if args.project == "ffmpeg":
                    configure = [str(source / "configure"), "--cc=clang-18",
                                 "--cxx=clang++-18", "--ld=clang-18",
                                 "--disable-autodetect", "--disable-doc",
                                 "--disable-debug"]
                    config_files = ("config.h", "config_components.h")
                    artifacts = ("ffmpeg", "ffprobe")
                else:
                    common = ["-f", str(source / "Makefile"), "LLVM=-18",
                              "LLVM_IAS=1", "ARCH=x86"]
                    configure = ["make", *common, "defconfig"]
                    config_files = (".config",)
                    artifacts = ("vmlinux", "arch/x86/boot/bzImage")
                end_to_end_start = time.monotonic()
                configuration = run(build, build / "configure.log", configure)
                configs = {p: digest(build / p) for p in config_files}
                configured_objects = sorted(str(p.relative_to(build))
                                            for p in build.rglob("*.o"))
                generation = None
                if mode == "ninja":
                    generation = run(build, build / "generate.log",
                                     [str(tool), "--ninja", f"-j{jobs}",
                                      *common, "all"])
                    command = ["sh", "./ninja.sh", f"-j{jobs}"]
                else:
                    command = ["make", f"-j{jobs}", *common, "all"]
                if args.trace_exec:
                    command = ["strace", "-f", "--seccomp-bpf", "-qq", "-s", "200",
                               "-e", "trace=execve", "-o", str(build / "execve.log"),
                               *command]
                new_objects = sorted(str(p.relative_to(build))
                                     for p in build.rglob("*.o")
                                     if str(p.relative_to(build)) not in configured_objects)
                if new_objects:
                    raise RuntimeError(f"Generation compiled objects: {new_objects[:10]}")
                result = run(build, build / "build.log", command)
                end_to_end_elapsed = time.monotonic() - end_to_end_start
                result.update(project=version, jobs=jobs, repeat=repeat + 1,
                              mode=mode, label=args.label, traced=args.trace_exec,
                              config_sha256=configs,
                              configuration=configuration,
                              end_to_end_wall_seconds=end_to_end_elapsed,
                              phase_total_wall_seconds=(configuration["wall_seconds"] +
                                  (generation["wall_seconds"] if generation else 0) +
                                  result["wall_seconds"]),
                              generation=generation, configured_objects=configured_objects,
                              artifacts={p: dict(bytes=(build / p).stat().st_size,
                                                 sha256=digest(build / p))
                                         for p in artifacts})
                if args.project == "ffmpeg":
                    run(build, build / "smoke.log",
                        [str(build / "ffmpeg"), "-v", "error", "-f", "lavfi",
                         "-i", "testsrc2=size=64x64:rate=10", "-t", "0.2",
                         "-c:v", "mpeg4", "-f", "null", "-"])
                else:
                    run(build, build / "smoke.log", ["file", str(build / artifacts[1])])
                with (root / "results.jsonl").open("a") as output:
                    output.write(json.dumps(result) + "\n")
    after = manifest(source)
    changed = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    (root / f"{args.label}-{args.project}-integrity.json").write_text(
        json.dumps(dict(files=len(before), changed=changed), indent=2) + "\n")
    if changed:
        raise RuntimeError(f"Source changes: {changed[:10]}")


if __name__ == "__main__":
    main()
