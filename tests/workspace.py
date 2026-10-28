#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Version-independent source synchronization, Ninja reuse and compression."""

import hashlib
import importlib.util
import fcntl
import io
import json
import os
from pathlib import Path
import shutil
import shlex
import signal
import subprocess
import tarfile
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get("OMKTR_BINARY", ROOT / "omktr")).resolve()
HELPER = ROOT / "tools/omktr_workspace.py"


class Workspace(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="omktr workspace ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        self.source = self.root / "package-1.0"
        self.source.mkdir()
        (self.source / "Makefile").write_text(
            "all: app\napp: a.o b.o\n\t@cat a.o b.o > $@\n"
            "a.o: a.c common.h\n\t@cat a.c common.h > $@; printf a >> runs\n"
            "b.o: b.c\n\t@cat b.c > $@; printf b >> runs\n")
        for name, value in (("a.c", "one\n"), ("b.c", "stable\n"), ("common.h", "header\n")):
            (self.source / name).write_text(value)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("KATI_") and
                    k not in ("MAKEFLAGS", "MAKEOVERRIDES", "OMKTR_WORKSPACE_ROOT")}
        self.env.update(CC="clang", CXX="clang++")

    def command(self, action, *args, env=None, success=True):
        command = [str(TOOL), "workspace", action, "--root", str(self.cache),
                   "--name", "category/package", *map(str, args)]
        result = subprocess.run(command, env=env or self.env, text=True,
                                capture_output=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def build(self, source=None, *args, env=None):
        options = ["--source", source] if source else []
        return self.command("run", *options, *args, "--", str(TOOL), "build", "-j4", env=env)

    def directory(self):
        return Path(self.command("path").stdout.strip())

    def profile(self):
        return self.directory().parent.parent

    def test_workspace_target_keeps_legacy_direct_execution(self):
        (self.source / "Makefile").write_text("workspace:\n\t@printf target > result\n")
        subprocess.run([str(TOOL), "workspace"], cwd=self.source, env=self.env,
                       check=True, capture_output=True, timeout=10)
        self.assertEqual((self.source / "result").read_text(), "target")

    def test_cache_is_private_even_with_a_permissive_build_umask(self):
        self.cache.mkdir(mode=0o755)
        subprocess.run([str(TOOL), "workspace", "run", "--root", str(self.cache),
                        "--name", "category/package", "--source", str(self.source),
                        "--", str(TOOL), "build", "-j1"],
                       env=self.env, umask=0, capture_output=True, check=True, timeout=30)
        directory = self.directory()
        profile, base = directory.parent.parent, directory.parent.parent.parent
        self.assertEqual(base.stat().st_mode & 0o077, 0)
        self.assertEqual(profile.stat().st_mode & 0o077, 0)
        base.chmod(0o777)
        result = self.command("status", success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not be writable by other users", result.stderr)

    def test_generated_source_ages_and_build_changes_survive_resynchronization(self):
        (self.source / "Makefile").write_text(
            "all: generated\ngenerated: template\n"
            "\t@cat template > generated; printf x >> generator-runs\n")
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode):
                (self.source / "template").write_text("old\n")
                (self.source / "generated").write_text("old\n")
                (self.source / "generated").chmod(0o644)
                os.utime(self.source / "template", ns=(100, 100))
                os.utime(self.source / "generated", ns=(200, 200))
                name = "generated/" + mode
                build = [str(TOOL), *(["build"] if mode == "ninja" else []), "-j4"]

                def synchronize_and_build():
                    self.command("run", "--name", name, "--source", self.source, "--", *build)

                synchronize_and_build()
                directory = Path(self.command("path", "--name", name).stdout.strip())
                self.assertFalse((directory / "generator-runs").exists(),
                                 "a current distributed generated source was unnecessarily rebuilt")
                (self.source / "template").write_text("new\n")
                os.utime(self.source / "template", ns=(300, 300))
                synchronize_and_build()
                self.assertEqual((directory / "generated").read_text(), "new\n")
                synchronize_and_build()
                self.assertEqual((directory / "generated").read_text(), "new\n")
                self.assertEqual((directory / "generator-runs").read_text(), "x")
                (self.source / "generated").chmod(0o755)
                synchronize_and_build()
                self.assertEqual((directory / "generated").read_text(), "new\n")
                self.assertEqual((directory / "generated").stat().st_mode & 0o777, 0o755)
                self.assertEqual((directory / "generator-runs").read_text(), "x")

    def test_source_directory_mode_changes_retain_generated_outputs(self):
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "input").write_text("source\n")
        (self.source / "Makefile").write_text(
            "all: nested/output\nnested/output: nested/input\n\t@cat $< > $@\n")
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode):
                nested.chmod(0o755)
                name = "directory/" + mode
                build = [str(TOOL), *(["build"] if mode == "ninja" else []), "-j4"]
                self.command("run", "--name", name, "--source", self.source, "--", *build)
                directory = Path(self.command("path", "--name", name).stdout.strip())
                stamp = (directory / "nested/output").stat().st_mtime_ns
                nested.chmod(0o750)
                self.command("run", "--name", name, "--source", self.source, "--", *build)
                self.assertEqual((directory / "nested/output").stat().st_mtime_ns, stamp)
                self.assertEqual((directory / "nested").stat().st_mode & 0o777, 0o750)

    def test_same_directory_and_new_version_reuse_unchanged_objects(self):
        self.build(self.source)
        directory = self.directory()
        before = {p: (directory / p).stat().st_mtime_ns for p in ("a.o", "b.o", "app")}
        self.build()
        self.assertEqual(before, {p: (directory / p).stat().st_mtime_ns for p in before})
        upgraded = self.root / "package-2.0"
        shutil.copytree(self.source, upgraded)
        # Release timestamps differ; identical inputs must retain workspace mtimes.
        for p in upgraded.iterdir():
            os.utime(p, ns=(123000000000, 123000000000))
        self.build(upgraded)
        self.assertEqual(directory, self.directory())
        self.assertEqual(before, {p: (directory / p).stat().st_mtime_ns for p in before})
        self.assertEqual(sorted((directory / "runs").read_text()), ["a", "b"])
        # A content change with a preserved source timestamp must still rebuild.
        stamp = (upgraded / "a.c").stat()
        (upgraded / "a.c").write_text("two\n")
        os.utime(upgraded / "a.c", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.build(upgraded)
        self.assertGreater((directory / "a.o").stat().st_mtime_ns, before["a.o"])
        self.assertEqual((directory / "b.o").stat().st_mtime_ns, before["b.o"])
        self.assertEqual((directory / "app").read_text(), "two\nheader\nstable\n")

    def test_header_changes_and_removed_sources_update_the_graph(self):
        self.build(self.source)
        directory = self.directory()
        other = (directory / "b.o").stat().st_mtime_ns
        (self.source / "common.h").write_text("new header\n")
        self.build(self.source)
        self.assertEqual((directory / "b.o").stat().st_mtime_ns, other)
        (self.source / "Makefile").write_text("all: app\napp: a.c\n\t@cat a.c > $@\n")
        (self.source / "b.c").unlink()
        self.build(self.source)
        self.assertFalse((directory / "b.c").exists())
        self.assertEqual((directory / "app").read_text(), "one\n")

    def test_changed_source_links_rebuild_even_with_older_referents(self):
        outside = self.root / "outside"
        outside.mkdir()
        for root in (self.source, outside):
            for name, value in (("one", "one\n"), ("two", "two\n")):
                folder = root / name
                folder.mkdir()
                (folder / "input").write_text(value)
        external_stamps = {p: p.stat().st_mtime_ns for p in outside.glob("*/input")}
        link = self.source / "selected"
        for mode in ("direct", "ninja"):
            for kind in ("file", "directory", "external"):
                with self.subTest(mode=mode, kind=kind):
                    link.unlink(missing_ok=True)
                    suffix = "/input" if kind == "directory" else ""
                    (self.source / "Makefile").write_text(
                        f"all: output\noutput: selected{suffix}\n\t@cat $< > $@\n")

                    def referent(version):
                        return (outside / version / "input" if kind == "external" else
                                Path(version) if kind == "directory" else Path(version) / "input")

                    link.symlink_to(referent("one"))
                    name = f"links/{mode}/{kind}"
                    build = [str(TOOL), *(["build"] if mode == "ninja" else []), "-j4"]

                    def run():
                        self.command("run", "--name", name, "--source", self.source, "--", *build)

                    run()
                    directory = Path(self.command("path", "--name", name).stdout.strip())
                    output = directory / "output"
                    self.assertEqual(output.read_text(), "one\n")
                    before = output.stat().st_mtime_ns
                    run()
                    self.assertEqual(output.stat().st_mtime_ns, before)
                    self.command("pack", "--name", name)
                    run()
                    self.assertEqual(output.stat().st_mtime_ns, before)
                    self.command("pack", "--name", name)
                    link.unlink()
                    link.symlink_to(referent("two"))
                    run()
                    self.assertEqual(output.read_text(), "two\n")
                    after = output.stat().st_mtime_ns
                    run()
                    self.assertEqual(output.stat().st_mtime_ns, after)
        self.assertEqual(external_stamps, {p: p.stat().st_mtime_ns for p in external_stamps})

    def test_removed_input_directory_never_follows_generated_parent_link(self):
        (self.source / "folder/empty").mkdir(parents=True)
        self.build(self.source)
        directory = self.directory()
        outside = self.root / "outside"
        (outside / "empty").mkdir(parents=True)
        shutil.rmtree(directory / "folder")
        (directory / "folder").symlink_to(outside)
        shutil.rmtree(self.source / "folder")
        result = self.command("run", "--source", self.source, "--", "true", success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside managed workspace", result.stderr)
        self.assertTrue((outside / "empty").is_dir())

    def test_configuration_flags_and_compiler_identity_select_separate_lanes(self):
        self.build(self.source)
        first = self.directory()
        self.build(self.source, env={**self.env, "CFLAGS": "-O0"})
        self.assertNotEqual(first, self.directory())
        self.build(self.source)
        self.assertEqual(first, self.directory())
        compiler = self.root / "compiler"
        compiler.write_text("#!/bin/sh\nexit 0\n")
        compiler.chmod(0o755)
        env = {**self.env, "CC": shlex.quote(str(compiler))}
        self.build(self.source, env=env)
        old = self.directory()
        stamp = compiler.stat()
        compiler.write_text("#!/bin/sh\nexit 1\n")
        os.utime(compiler, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.build(self.source, env=env)
        self.assertNotEqual(old, self.directory())

    def test_compression_restores_ninja_state_and_nanosecond_timestamps(self):
        (self.source / "large-source").write_bytes(b"retained source line\n" * 100_000)
        (self.source / "alias").symlink_to("a.c")
        self.build(self.source)
        directory, profile = self.directory(), self.profile()
        before = {p: (directory / p).stat().st_mtime_ns for p in ("a.c", "a.o", "b.o", "app", "runs")}
        result = json.loads(self.command("pack").stdout)
        self.assertLess(result["compressed_bytes"], result["original_bytes"] / 4)
        self.assertFalse((directory / "a.o").exists())
        self.assertTrue((profile / "content.tar.xz").exists())
        self.build()
        self.assertEqual(before, {p: (directory / p).stat().st_mtime_ns for p in before})
        self.assertEqual(os.readlink(directory / "alias"), "a.c")
        self.assertFalse((profile / "content.tar.xz").exists())

    def test_explicit_formats_and_legacy_archives_restore_build_state(self):
        (self.source / "alias").symlink_to("a.c")
        (self.source / "a.c").chmod(0o751)
        self.build(self.source)
        directory, profile = self.directory(), self.profile()
        expected_runs = (directory / "runs").read_text()
        for compression, suffix in (("xz", "xz"), ("zstd", "zst")):
            with self.subTest(compression=compression):
                before = {p.name: (p.lstat().st_mode, p.lstat().st_mtime_ns)
                          for p in directory.iterdir()}
                result = json.loads(self.command("pack", "--compression=" + compression).stdout)
                self.assertEqual(result["compression"], compression)
                archive = profile / ("content.tar." + suffix)
                self.assertTrue(archive.exists())
                state_path = profile / "state.json"
                state = json.loads(state_path.read_text())
                self.assertEqual(state["archive_format"], compression)
                if compression == "xz":
                    # A pre-upgrade workspace has no archive_format field.
                    state.pop("archive_format")
                    state_path.write_text(json.dumps(state))
                self.command("unpack")
                self.assertEqual(before, {p.name: (p.lstat().st_mode, p.lstat().st_mtime_ns)
                                          for p in directory.iterdir()})
                self.assertEqual(os.readlink(directory / "alias"), "a.c")
                self.assertFalse(archive.exists())
                self.build()
                self.assertEqual((directory / "runs").read_text(), expected_runs)

    def test_zstd_corruption_and_unknown_format_preserve_archive(self):
        self.build(self.source)
        profile = self.profile()
        self.command("pack", "--compression=zstd")
        archive = profile / "content.tar.zst"
        state_path = profile / "state.json"
        state = json.loads(state_path.read_text())
        state["archive_format"] = "unknown"
        state_path.write_text(json.dumps(state))
        self.assertIn("unsupported workspace archive format",
                      self.command("unpack", success=False).stderr)
        self.assertIn("unsupported workspace archive format",
                      self.command("pack", success=False).stderr)
        state["archive_format"] = "zstd"
        state_path.write_text(json.dumps(state))
        archive.write_bytes(b"corrupt" + archive.read_bytes()[7:])
        self.assertIn("checksum mismatch", self.command("unpack", success=False).stderr)
        self.assertTrue(archive.exists())
        self.assertFalse((profile / "content/source/app").exists())

    def test_zstd_distribution_command_roundtrip_and_failed_publication(self):
        self.build(self.source)
        profile = self.profile()
        state_path = profile / "state.json"
        original_state = state_path.read_bytes()
        spec = importlib.util.spec_from_file_location("workspace_helper", HELPER)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        with mock.patch.object(helper, "has_zstd_module", return_value=False):
            state = json.loads(original_state)
            with mock.patch.object(helper.shutil, "which", return_value=None):
                with self.assertRaisesRegex(RuntimeError, "compression.zstd module or the zstd command"):
                    helper.pack(profile, state, "zstd")
            self.assertEqual(original_state, state_path.read_bytes())
            with mock.patch.object(helper.subprocess, "run", return_value=mock.Mock(
                    returncode=1, stderr=b"injected compressor failure")):
                with self.assertRaisesRegex(RuntimeError, "injected compressor failure"):
                    helper.pack(profile, state, "zstd")
            self.assertEqual(original_state, state_path.read_bytes())
            self.assertFalse((profile / "content.tar.zst").exists())
            before = {str(p.relative_to(profile / "content")): (p.read_bytes(), p.stat().st_mtime_ns)
                      for p in (profile / "content").rglob("*") if p.is_file()}
            helper.pack(profile, state, "zstd")
        # Exercise CLI -> stdlib and stdlib -> CLI interoperability wherever
        # Python supplies the module; Alpine exercises CLI in both directions.
        helper.unpack(profile, state)
        helper.pack(profile, state, "zstd")
        with mock.patch.object(helper, "has_zstd_module", return_value=False):
            helper.unpack(profile, state)
        self.assertEqual(before, {str(p.relative_to(profile / "content")): (p.read_bytes(), p.stat().st_mtime_ns)
                                  for p in (profile / "content").rglob("*") if p.is_file()})

    def test_subsecond_inputs_rebuild_with_a_shell_rejecting_test_nt(self):
        temporary_shell = tempfile.TemporaryDirectory(prefix="omktr-precision-")
        self.addCleanup(temporary_shell.cleanup)
        shell = Path(temporary_shell.name) / "shell"
        shell.write_text("#!/bin/sh\ncase \"$*\" in *' -nt '*) exit 92 ;; esac\nexec /bin/sh \"$@\"\n")
        shell.chmod(0o755)
        (self.source / "Makefile").write_text(
            f"SHELL := {shell}\nall: result\nresult: input\n\t@cat input > result\n")
        (self.source / "input").write_text("old")
        self.build(self.source)
        directory = self.directory()
        (directory / "input").write_text("new")
        stamp = time.time_ns() // 10**9 * 10**9 - 10**9
        os.utime(directory / "result", ns=(stamp + 100_000_000, stamp + 100_000_000))
        os.utime(directory / "input", ns=(stamp + 200_000_000, stamp + 200_000_000))
        with (directory / "Makefile").open("a") as makefile:
            makefile.write("\n# Regenerate while the previous output exists.\n")
        self.build()
        self.assertEqual((directory / "result").read_text(), "new")

    def test_interrupted_pack_keeps_expanded_files_and_metadata(self):
        self.build(self.source)
        directory, profile = self.directory(), self.profile()
        state_path = profile / "state.json"
        before = state_path.read_bytes()
        spec = importlib.util.spec_from_file_location("workspace_helper", HELPER)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        for compression in ("xz", "zstd"):
            with self.subTest(compression=compression):
                with mock.patch.object(helper, "restore_archive", side_effect=KeyboardInterrupt):
                    with self.assertRaises(KeyboardInterrupt):
                        helper.pack(profile, json.loads(before), compression)
                self.assertEqual(before, state_path.read_bytes())
                self.assertTrue((directory / "app").is_file())
                self.assertEqual(list(profile.glob(".archive-*")), [])
                self.assertEqual(list(profile.glob("content.tar.*")), [])

    def test_compression_options_are_validated_before_using_workspace(self):
        result = self.command("pack", "--compression=invalid", success=False)
        self.assertEqual(result.returncode, 2)
        result = self.command("run", "--compression=zstd", success=False)
        self.assertEqual(result.returncode, 2)

    def test_gc_records_selected_format_without_implicitly_repacking(self):
        self.build(self.source)
        profile = self.profile()
        self.command("gc", "--idle-seconds=0", "--compression=zstd")
        archive = profile / "content.tar.zst"
        before = archive.read_bytes()
        self.assertEqual(json.loads(self.command("status").stdout)["archive_format"], "zstd")
        result = json.loads(self.command("pack", "--compression=xz").stdout)
        self.assertEqual(result, dict(already_packed=True, compression="zstd"))
        self.assertEqual(before, archive.read_bytes())
        self.build()

    def test_corrupt_archive_fails_without_publishing_partial_restore(self):
        self.build(self.source)
        profile = self.profile()
        self.command("pack")
        archive = profile / "content.tar.xz"
        archive.write_bytes(b"corrupt" + archive.read_bytes()[7:])
        result = self.command("unpack", success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum mismatch", result.stderr)
        self.assertFalse((profile / "content/source/app").exists())
        self.assertTrue(archive.exists())

    def test_hard_linked_outputs_are_retained_without_lossy_compression(self):
        self.build(self.source)
        directory, profile = self.directory(), self.profile()
        os.link(directory / "app", directory / "alias")
        result = self.command("pack", success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hard-linked", result.stderr)
        self.assertEqual((directory / "app").stat().st_ino, (directory / "alias").stat().st_ino)
        self.assertFalse((profile / "content.tar.xz").exists())

    def test_archive_path_traversal_is_rejected(self):
        self.build(self.source)
        profile = self.profile()
        self.command("pack")
        archive = profile / "content.tar.xz"
        manifest = dict(entries={"../escape": dict(kind="file", mode=0o644,
                                                 identity=[0, 0, 0, 1], sha256="bad")}, inputs={})
        with tarfile.open(archive, "w:xz") as out:
            data = json.dumps(manifest).encode()
            member = tarfile.TarInfo("manifest.json")
            member.size = len(data)
            out.addfile(member, io.BytesIO(data))
            member = tarfile.TarInfo("payload/../escape")
            member.size = 1
            out.addfile(member, io.BytesIO(b"x"))
        state = json.loads((profile / "state.json").read_text())
        with archive.open("rb") as stream:
            state["archive_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        (profile / "state.json").write_text(json.dumps(state))
        self.assertNotEqual(self.command("unpack", success=False).returncode, 0)
        self.assertFalse((profile / "escape").exists())
        self.assertFalse((profile / "content/source/app").exists())

    def test_child_status_and_failed_configuration_are_preserved(self):
        result = self.command("run", "--source", self.source, "--configure", "exit 7",
                              "--", "sh", "-c", "printf bad > ran", success=False)
        self.assertEqual(result.returncode, 7)
        self.assertFalse((self.directory() / "ran").exists())
        retry = self.command("run", "--", "sh", "-c", "printf bad > ran", success=False)
        self.assertEqual(retry.returncode, 7)
        self.assertFalse((self.directory() / "ran").exists())
        self.build(self.source, "--configure", "printf configured > configured")
        self.assertTrue((self.directory() / "configured").exists())
        result = self.command("run", "--configure", "exit 9", "--", "true", success=False)
        self.assertEqual(result.returncode, 9)
        self.assertEqual(self.command("run", "--", "true", success=False).returncode, 9)
        self.build(None, "--configure", "")

    def test_real_compiler_depfiles_recipe_changes_and_restored_noop(self):
        makefile = (
            "BIAS := 0\nall: app\napp: a.o b.o\n\t@$(CC) a.o b.o -o app\n"
            "a.o: .KATI_DEPFILE = a.d\na.o: a.c\n"
            "\t@$(CC) -MMD -MF a.d -c a.c -o a.o\n"
            "b.o: .KATI_DEPFILE = b.d\nb.o: b.c\n"
            "\t@$(CC) -MMD -MF b.d -DBIAS=$(BIAS) -c b.c -o b.o\n")
        (self.source / "Makefile").write_text(makefile)
        (self.source / "a.c").write_text(
            '#include "common.h"\n#include <stdio.h>\n'
            'int b(void); int main(void) { printf("%d\\n", VALUE + b()); }\n')
        (self.source / "b.c").write_text("int b(void) { return 2 + BIAS; }\n")
        (self.source / "common.h").write_text("#define VALUE 1\n")
        self.build(self.source)
        directory = self.directory()
        before = {p: (directory / p).stat().st_mtime_ns for p in ("a.o", "b.o", "app")}
        self.command("pack")
        self.build()
        self.assertEqual(before, {p: (directory / p).stat().st_mtime_ns for p in before})
        upgraded = self.root / "package-2.0"
        shutil.copytree(self.source, upgraded)
        (upgraded / "common.h").write_text("#define VALUE 3\n")
        self.build(upgraded)
        self.assertGreater((directory / "a.o").stat().st_mtime_ns, before["a.o"])
        self.assertEqual((directory / "b.o").stat().st_mtime_ns, before["b.o"])
        (upgraded / "Makefile").write_text(makefile.replace("BIAS := 0", "BIAS := 7"))
        self.build(upgraded)
        self.assertEqual(subprocess.check_output([str(directory / "app")], text=True), "12\n")
        self.command("run", "--name", "fresh/control", "--source", upgraded,
                     "--", str(TOOL), "build", "-j4")
        fresh = Path(self.command("path", "--name", "fresh/control").stdout.strip())
        for name in ("a.o", "b.o", "app"):
            self.assertEqual((directory / name).read_bytes(), (fresh / name).read_bytes())

    def test_global_exports_invalidate_retained_recipe_outputs(self):
        for depfile in (False, True):
            with self.subTest(depfile=depfile):
                name = "exports/" + str(depfile)

                def build():
                    self.command("run", "--name", name, "--source", self.source,
                                 "--", str(TOOL), "build", "-j4")

                for value in ("old", "new", "third", "quoted '$ \\ slash\nlast line\n"):
                    metadata = "output: .KATI_DEPFILE = output.d\n" if depfile else ""
                    dependency = "; printf 'output: input\\n' > output.d" if depfile else ""
                    definition = value.replace("$", "$$")
                    (self.source / "Makefile").write_text(
                        f"define VALUE\n{definition}\nendef\nexport VALUE\nall: output\n{metadata}"
                        "output: input\n\t@printf '%s' \"$$VALUE\" > output; "
                        f"printf x >> runs{dependency}\n")
                    (self.source / "input").write_text("unchanged\n")
                    build()
                    directory = Path(self.command("path", "--name", name).stdout.strip())
                    self.assertEqual((directory / "output").read_text(), value)
                    stamp = (directory / "output").stat().st_mtime_ns
                    runs = (directory / "runs").read_text()
                    build()
                    self.assertEqual((directory / "output").stat().st_mtime_ns, stamp)
                    self.assertEqual((directory / "runs").read_text(), runs)
                self.assertEqual((directory / "runs").read_text(), "xxxx")
                self.command("pack", "--name", name)
                build()
                self.assertEqual((directory / "output").stat().st_mtime_ns, stamp)
                self.assertEqual((directory / "runs").read_text(), "xxxx")

    def test_elided_dummy_recipe_retains_identity_for_later_changes(self):
        (self.source / "Makefile").write_text("all: output\noutput:\n\t@:\n")
        (self.source / "output").write_text("distributed\n")
        self.build(self.source)
        directory = self.directory()
        stamp = (directory / "output").stat().st_mtime_ns
        subprocess.run(["ninja", "-f", ".openmktr/build.ninja", "-t", "clean"],
                       cwd=directory, check=True, capture_output=True)
        self.assertEqual((directory / "output").stat().st_mtime_ns, stamp)
        self.command("pack")
        self.build()
        self.assertEqual((directory / "output").stat().st_mtime_ns, stamp)
        (self.source / "Makefile").write_text(
            "all: output\noutput:\n\t@printf rebuilt > output; printf x >> runs\n")
        self.build(self.source)
        self.assertEqual((directory / "output").read_text(), "rebuilt")
        self.build(self.source)
        self.assertEqual((directory / "runs").read_text(), "x")

    def test_global_export_tracking_preserves_compiler_depfile_inference(self):
        (self.source / "Makefile").write_text(
            "export LABEL := retained\nall: a.o\na.o: a.c\n"
            "\t@$(CC) -MMD -MF a.d -c a.c -o a.o\n")
        (self.source / "a.c").write_text('#include "common.h"\nint value = VALUE;\n')
        (self.source / "common.h").write_text("#define VALUE 1\n")

        def build():
            self.command("run", "--source", self.source, "--", str(TOOL),
                         "build", "--detect_depfiles", "-j4")

        build()
        directory = self.directory()
        self.assertIn("omktr_depfile = a.d.tmp\n",
                      (directory / ".openmktr/build.ninja").read_text())
        original = (directory / "a.o").read_bytes()
        stamp = (directory / "a.o").stat().st_mtime_ns
        build()
        self.assertEqual((directory / "a.o").stat().st_mtime_ns, stamp)
        (self.source / "common.h").write_text("#define VALUE 2\n")
        build()
        self.assertNotEqual((directory / "a.o").read_bytes(), original)

    def test_out_of_tree_configuration_reuses_unchanged_release(self):
        configure = ('printf x >> configurations; '
                     'printf "all: app\\napp: ../source/a.c\\n\\t@cat ../source/a.c > app\\n" > Makefile')
        self.build(self.source, "--out-of-tree", "--configure", configure)
        directory = self.directory()
        self.assertEqual(directory.name, "build")
        stamp = (directory / "app").stat().st_mtime_ns
        upgraded = self.root / "package-2.0"
        shutil.copytree(self.source, upgraded)
        self.build(upgraded)
        self.assertEqual((directory / "configurations").read_text(), "x")
        self.assertEqual((directory / "app").stat().st_mtime_ns, stamp)
        (upgraded / "a.c").write_text("changed\n")
        self.build(upgraded)
        self.assertEqual((directory / "configurations").read_text(), "xx")
        self.assertEqual((directory / "app").read_text(), "changed\n")

    def test_active_build_excludes_gc_and_forwards_cancellation(self):
        self.build(self.source)
        directory = self.directory()
        process = subprocess.Popen(
            [str(TOOL), "workspace", "run", "--root", str(self.cache),
             "--name", "category/package", "--", "sh", "-c",
             "trap 'printf stopped > stopped; exit 0' TERM; "
             "printf ready > ready; while :; do sleep 1; done"],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        deadline = time.monotonic() + 10
        while not (directory / "ready").exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue((directory / "ready").exists())
        result = json.loads(self.command("gc", "--idle-seconds", "0").stdout)
        self.assertEqual(result["packed_workspaces"], 0)
        self.assertTrue((directory / "app").exists())
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 143, stdout + stderr)
        self.assertEqual((directory / "stopped").read_text(), "stopped")
        result = json.loads(self.command("gc", "--idle-seconds", "0").stdout)
        self.assertEqual(result["packed_workspaces"], 1)
        self.build()
        self.assertTrue((directory / "app").exists())

    def test_portage_phase_refreshes_idle_age_without_releasing_ownership(self):
        builddir = self.root / "portage/package-1"
        builddir.parent.mkdir()
        lockfile = builddir.parent / ".package-1.portage_lockfile"
        env = dict(self.env, CATEGORY="category", PN="lease", SLOT="0", CHOST="target",
                   CBUILD="target", EAPI="8", EPREFIX="", S=str(self.source), B=str(self.source),
                   WORKDIR=str(self.root), PORTAGE_BUILDDIR=str(builddir), DEPEND="", BDEPEND="")
        with lockfile.open("w") as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            source = Path(self.command("portage-prepare", env=env).stdout.splitlines()[0])
            statefile = source.parent.parent / "state.json"
            state = json.loads(statefile.read_text())
            state["last_used"] = 0  # Simulate a source phase lasting over an hour.
            statefile.write_text(json.dumps(state))
            started = time.time()
            self.command("portage-enter", env={**env, "EBUILD_PHASE": "install"})
            state = json.loads(statefile.read_text())
            self.assertGreaterEqual(state["last_used"], started)
            self.assertEqual(state["owner"], str(lockfile))
        self.assertEqual(json.loads(self.command("gc", "--idle-seconds", "3600").stdout),
                         {"packed_workspaces": 0})

    def test_interrupted_source_sync_requires_recovery_and_removes_partial_inputs(self):
        self.build(self.source)
        directory = self.directory()
        before = (directory / "runs").read_text()
        upgraded = self.root / "package-2.0"
        shutil.copytree(self.source, upgraded)
        (upgraded / "00-new.c").write_text("new input\n")
        (upgraded / "a.c").write_text("two\n")
        startup = self.root / "fault-injection"
        startup.mkdir()
        (startup / "sitecustomize.py").write_text(
            "import os, signal\n"
            "replace = os.replace\n"
            "count = 0\n"
            "def interrupt(source, target):\n"
            "    global count\n"
            "    replace(source, target)\n"
            "    if os.path.basename(source).startswith('.input-'):\n"
            "        count += 1\n"
            "        if count == 2: os.kill(os.getpid(), signal.SIGTERM)\n"
            "os.replace = interrupt\n")
        result = self.command("run", "--source", upgraded, "--", str(TOOL), "build",
                              env={**self.env, "PYTHONPATH": str(startup)}, success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((directory / "00-new.c").exists())
        self.assertEqual((directory / "a.c").read_text(), "two\n")
        self.assertTrue(json.loads(self.command("status").stdout)["synchronization_pending"])
        result = self.command("run", "--", str(TOOL), "build", success=False)
        self.assertIn("source synchronization incomplete", result.stderr)
        self.assertNotEqual(self.command("pack", success=False).returncode, 0)
        self.assertEqual((directory / "runs").read_text(), before)
        # A rollback must restore the original input even though its incoming
        # hash equals the last successful synchronization's hash.
        self.build(self.source)
        self.assertEqual((directory / "app").read_text(), "one\nheader\nstable\n")
        self.assertFalse((directory / "00-new.c").exists())
        result = self.command("run", "--source", upgraded, "--", str(TOOL), "build",
                              env={**self.env, "PYTHONPATH": str(startup)}, success=False)
        self.assertNotEqual(result.returncode, 0)
        recovered = self.root / "package-3.0"
        shutil.copytree(self.source, recovered)
        (recovered / "a.c").write_text("three\n")
        (recovered / "b.c").write_text("new stable\n")
        self.build(recovered)
        self.assertFalse((directory / "00-new.c").exists())
        self.assertFalse((directory.parent.parent / ".sync").exists())
        self.assertEqual((directory / "app").read_text(), "three\nheader\nnew stable\n")


if __name__ == "__main__":
    unittest.main()
