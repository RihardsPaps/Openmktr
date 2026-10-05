#!/usr/bin/env python3
"""GNU Make compatibility regressions found while reviewing PR 19."""

import gzip
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
KATI = Path(os.environ.get("OMKTR_BINARY", ROOT / "omktr")).resolve()
CASES = json.loads(Path(__file__).with_suffix(".json").read_text())


def run(command, directory, environment, stdin=None):
    process = subprocess.Popen(
        command, cwd=directory, env=environment, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(stdin, timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        raise AssertionError(f"{command} did not terminate: {output[-2000:]}")
    return process.returncode, output


class MakeCompatibilityRegressions(unittest.TestCase):
    def test_ninja_vpath_provider_tracks_later_source_and_prerequisite_changes(self):
        with tempfile.TemporaryDirectory(prefix="kati-vpath-ninja-") as temp:
            directory = Path(temp)
            source = directory / "src" / "item"
            source.parent.mkdir()
            source.write_text("old\n")
            prerequisite = directory / "input"
            prerequisite.write_text("input\n")
            os.utime(source, (1000000100, 1000000100))
            os.utime(prerequisite, (1000000000, 1000000000))
            (directory / "Makefile").write_text(
                "VPATH = src\n"
                "all: item\n\t@cat $< > result\n"
                "item: input\n\t@echo rebuilt > $@\n"
            )
            environment = dict(os.environ)
            environment.pop("MAKEFLAGS", None)
            environment.pop("MFLAGS", None)

            status, output = run([str(KATI), "--ninja", "-j1", "all"],
                                 directory, environment)
            self.assertEqual(status, 0, output)

            def build():
                status, output = run(["sh", "ninja.sh", "-j1"], directory,
                                     environment)
                self.assertEqual(status, 0, output)
                return (directory / "result").read_text()

            self.assertEqual(build(), "old\n")
            self.assertTrue((directory / "item").is_symlink())
            source.write_text("new\n")
            os.utime(source, (1000000200, 1000000200))
            self.assertEqual(build(), "new\n")
            self.assertTrue((directory / "item").is_symlink())
            # Regeneration must retain the provider identity of the link
            # created by the previous graph, including its unlink guard.
            status, output = run([str(KATI), "--ninja", "-j1", "all"],
                                 directory, environment)
            self.assertEqual(status, 0, output)
            prerequisite.write_text("changed\n")
            os.utime(prerequisite, (1000000300, 1000000300))
            self.assertEqual(build(), "rebuilt\n")
            self.assertFalse((directory / "item").is_symlink())
            self.assertEqual(source.read_text(), "new\n")
            source.unlink()
            self.assertEqual(build(), "rebuilt\n")

    def test_ninja_removed_vpath_provider_rebuilds_dangling_link(self):
        with tempfile.TemporaryDirectory(prefix="kati-vpath-missing-") as temp:
            directory = Path(temp)
            source = directory / "src" / "item"
            source.parent.mkdir()
            source.write_text("source\n")
            (directory / "input").write_text("input\n")
            os.utime(source, (1000000100, 1000000100))
            os.utime(directory / "input", (1000000000, 1000000000))
            (directory / "Makefile").write_text(
                "VPATH = src\nall: item\n\t@cat $< > result\n"
                "item: input\n\t@echo rebuilt > $@\n")
            environment = dict(os.environ)
            environment.pop("MAKEFLAGS", None)
            environment.pop("MFLAGS", None)
            for remove_source in (False, True):
                if remove_source:
                    source.unlink()
                status, output = run([str(KATI), "--ninja", "all"],
                                     directory, environment)
                self.assertEqual(status, 0, output)
                status, output = run(["sh", "ninja.sh"], directory, environment)
                self.assertEqual(status, 0, output)
                self.assertEqual((directory / "result").read_text(),
                                 "rebuilt\n" if remove_source else "source\n")
            self.assertFalse((directory / "item").is_symlink())

    def test_parallel_intermediate_is_reconsidered_for_waiting_parent(self):
        with tempfile.TemporaryDirectory(prefix="kati-intermediate-race-") as temp:
            directory = Path(temp)
            inputs = [f"deps/d{i}" for i in range(3000)]
            (directory / "deps").mkdir()
            for filename in ["source", "a.out", *inputs]:
                path = directory / filename
                path.write_text("old\n")
                os.utime(path, (1000000000, 1000000000))
            os.utime(directory / "a.out", (1000000100, 1000000100))
            (directory / "Makefile").write_text(
                ".INTERMEDIATE: shared.mid\nall: a.out b.out\n"
                "a.out: shared.mid\n\t@cat shared.mid > $@\n"
                "b.out: shared.mid | gate\n\t@cat shared.mid > $@\n"
                "gate:\n\t@sleep 0.003\nshared.mid: source " +
                " ".join(inputs) + "\n\t@cp $< $@\n")
            environment = dict(os.environ)
            environment.pop("MAKEFLAGS", None)
            environment.pop("MFLAGS", None)
            for iteration in range(20):
                with self.subTest(iteration=iteration):
                    for filename in ("shared.mid", "b.out"):
                        (directory / filename).unlink(missing_ok=True)
                    status, output = run([str(KATI), "-j2", "all"],
                                         directory, environment)
                    self.assertEqual(status, 0, output)
                    self.assertEqual((directory / "b.out").read_text(), "old\n")

    def test_make_cases(self):
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MAKE", "KATI")) and key != "MFLAGS"
        }
        for case in CASES:
            with self.subTest(case=case["name"]):
                with tempfile.TemporaryDirectory(prefix="kati-make-compat-") as temp:
                    directory = Path(temp)
                    files = dict(case.get("files", {}))
                    if not case.get("stdin"):
                        files["Makefile"] = case["text"]
                    for filename, content in files.items():
                        path = directory / filename
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(content)
                    for filename, timestamp in case.get("times", {}).items():
                        os.utime(directory / filename, (timestamp, timestamp))

                    case_env = dict(environment, **case.get("extra_env", {}))
                    if case["name"] == "fortran_missing_default":
                        # Make the missing compiler deterministic on hosts with
                        # or without a system f77 installation.
                        compiler = directory / "bin" / "f77"
                        compiler.parent.mkdir()
                        compiler.write_text("#!/bin/sh\nexit 42\n")
                        compiler.chmod(0o755)
                        case_env["PATH"] = f"{compiler.parent}:{case_env['PATH']}"
                        case_env.pop("FC", None)
                    command = [str(KATI), "-j1", "-f",
                               "-" if case.get("stdin") else "Makefile"]
                    command += case.get("args", ["all"])
                    if case.get("ninja"):
                        command.insert(1, "--ninja")
                    status, output = run(
                        command, directory, case_env,
                        case["text"] if case.get("stdin") else None,
                    )
                    self.assertEqual(status == 0, case["expected_status"] == 0,
                                     f"{case['name']}: {output}")
                    if case["name"] == "fortran_missing_default":
                        self.assertIn("f77", output)
                    if case["name"] in ("no_builtin_suffix",
                                        "builtin_removed_suffix"):
                        self.assertIn("No rule to make target", output)
                    if case.get("ninja") and status == 0:
                        status, output = run(
                            ["sh", "ninja.sh", "-j1"], directory, case_env)
                        self.assertEqual(status, 0, f"{case['name']}: {output}")
                    for filename, expected in case["expected_files"].items():
                        path = directory / filename
                        actual = path.read_text(errors="replace") if path.is_file() else None
                        self.assertEqual(actual, expected,
                                         f"{case['name']}: {filename}; {output}")

    def test_openwrt_boot_uses_new_compressed_image(self):
        from validation.boot_openwrt import refresh_image

        with tempfile.TemporaryDirectory(prefix="kati-image-refresh-") as temp:
            directory = Path(temp)
            compressed = directory / "new.img.gz"
            image = directory / "boot.img"
            image.write_bytes(b"old boot from prior run")
            with gzip.open(compressed, "wb") as output:
                output.write(b"current build")
            refresh_image(compressed, image)
            self.assertEqual(image.read_bytes(), b"current build")

    def test_xorg_verifier_rejects_system_library_fallback(self):
        from validation.verify_xorg import linked_from_prefix

        with tempfile.TemporaryDirectory(prefix="kati-xorg-check-") as temp:
            prefix = Path(temp)
            library = prefix / "lib"
            library.mkdir()
            (library / "libX11.so.6").touch()
            bindings = (f"libX11.so.6 => {library / 'libX11.so.6'} (0x0)\n"
                        "libXext.so.6 => /usr/lib/libXext.so.6 (0x0)\n")
            self.assertTrue(linked_from_prefix(bindings, prefix, "libX11.so"))
            self.assertFalse(linked_from_prefix(bindings, prefix, "libXext.so"))


if __name__ == "__main__":
    unittest.main()
