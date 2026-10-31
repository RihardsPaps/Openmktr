#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Focused producer/consumer checks for runtime Ninja recipe expansion."""

import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


BINARY = Path(os.environ.get(
    "OMKTR_BINARY", Path(__file__).resolve().parents[1] / "omktr"
)).resolve()


class GeneratedRecipeReads(unittest.TestCase):
    def test_deferred_recursion_preserves_implicit_makeflags_reads(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "includes").mkdir()
                (root / "includes/common.mk").write_text("build := -f leaf.mk obj\n")
                (root / "seed").write_text("ready")
                (root / "Makefile").write_text(
                    "MAKEFLAGS += --include-dir=includes\n"
                    "all: result\nresult: seed\n"
                    "\t@test '$(shell cat seed)' = ready; $(MAKE) -f child.mk\n")
                (root / "child.mk").write_text(
                    "include common.mk\nall:\n\t@$(MAKE) $(build)=sub\n")
                (root / "leaf.mk").write_text(
                    "all:\n\t@printf '%s' '$(obj):$(origin MARK):$(MARK)' > result\n")
                result = subprocess.run(
                    [str(BINARY), "build", "--graph_optimizations=" + profile,
                     "-j4", "MARK=kept", "all"], cwd=root,
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((root / "result").read_text(), "sub:command line:kept")

    def test_long_append_context_preserves_order_and_deferred_values(self):
        for dynamic in (False, True):
            with self.subTest(dynamic=dynamic), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                definitions = "WORD = $(later)\n" + "WORD += x\n" * 3000
                expression = "$(call $(selected))" if dynamic else "$(reader)"
                (root / "Makefile").write_text(
                    definitions + "later = first\nselected = reader\n"
                    "reader = $(shell cat seed)\nall: result\nresult: seed\n"
                    "\t@printf '%s' '" + expression + ":$(WORD)' > $@\n")
                result = subprocess.run([str(BINARY), "build", "-j4", "all"],
                                        cwd=root, capture_output=True, text=True,
                                        timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((root / "result").read_text(),
                                 "ready:first" + " x" * 3000)
                (root / "seed").write_text("changed")
                result = subprocess.run([str(BINARY), "build", "-j4", "all"],
                                        cwd=root, capture_output=True, text=True,
                                        timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((root / "result").read_text(),
                                 "changed:first" + " x" * 3000)

    def test_shared_context_is_sealed_and_changed_contexts_fall_back(self):
        import fcntl
        import sys
        if not hasattr(os, "memfd_create") or not hasattr(fcntl, "F_ADD_SEALS"):
            self.skipTest("sealed Linux context snapshots unavailable")
        try:
            probe_fd = os.memfd_create("omktr-test", os.MFD_ALLOW_SEALING)
            os.close(probe_fd)
        except OSError:
            self.skipTest("memfd is restricted on this host")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "Makefile").write_text(
                "export KATI_RUNTIME_CONTEXT_FD := invalid\n"
                "selected = reader\nreader = $(shell cat seed)\nUNUSED = " +
                "x" * 600000 + "\nall: result\nresult: seed\n"
                "\t@test \"$${KATI_RUNTIME_CONTEXT_FD:-absent}\" != invalid && "
                "printf '%s' '$(call $(selected))' > $@\n")
            subprocess.run([str(BINARY), "--ninja", "-j32", "all"], cwd=root,
                           capture_output=True, check=True, timeout=30)
            sidecar = next(p for p in root.glob(".kati_runtime-*")
                           if not p.name.startswith((".kati_runtime-context-",
                                                     ".kati_runtime-reads-")))
            context = max(root.glob(".kati_runtime-context-*"), key=lambda p: p.stat().st_size)
            original = context.read_bytes()
            launched = subprocess.run(["sh", "./ninja.sh", "-j32"], cwd=root,
                                      capture_output=True, text=True, timeout=30)
            self.assertEqual(launched.returncode, 0, launched.stdout + launched.stderr)
            self.assertEqual((root / "result").read_text(), "ready")
            (root / "result").unlink()
            fallback = subprocess.run(["ninja", "-j32"], cwd=root,
                                      capture_output=True, text=True, timeout=30)
            self.assertEqual(fallback.returncode, 0, fallback.stdout + fallback.stderr)
            self.assertEqual((root / "result").read_text(), "ready")
            script = r'''
import errno,fcntl,os,struct,subprocess,sys
from pathlib import Path
binary,sidecar,context,mode=sys.argv[1:5]
descriptor=int(os.environ['KATI_RUNTIME_CONTEXT_FD'])
seals=fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL
assert fcntl.fcntl(descriptor,fcntl.F_GET_SEALS)&seals==seals
try:
 os.write(descriptor,b'corrupt')
except OSError as exc:
 assert exc.errno==errno.EPERM
else:raise AssertionError('snapshot is writable')
def parts(data):
 result=[]
 while data:
  length=struct.unpack_from('<Q',data)[0];result.append(data[8:8+length]);data=data[8+length:]
 return result
def pack(fields):return b''.join(struct.pack('<Q',len(x))+x for x in fields)
def cache(payload):
 checksum=14695981039346656037
 for byte in payload:checksum=((checksum^byte)*1099511628211)&((1<<64)-1)
 return pack([b'OMKTR_GRAPH1',str(checksum).encode(),payload])
env=dict(os.environ)
if mode in ('changed','corrupt'):
 definitions=parts(parts(Path(context).read_bytes())[2])
 for i,definition in enumerate(definitions):
  fields=parts(definition)
  if fields[0]==(b'reader' if mode=='changed' else b'UNUSED'):
   if mode=='changed':fields[2:]=[b'simple',b'changed',b'changed']
   else:fields[3]=pack([b'unknown-expression-kind'])
   definitions[i]=pack(fields);break
 else:raise AssertionError('definition missing')
 Path(context).write_bytes(cache(pack(definitions)))
if mode=='unsealed':
 descriptor=os.open(context,os.O_RDONLY);env['KATI_RUNTIME_CONTEXT_FD']=str(descriptor)
child=subprocess.run([binary,'--internal-evaluate-recipe',sidecar],env=env,
                     pass_fds=(descriptor,),capture_output=True,text=True,timeout=20)
if mode=='corrupt':
 assert child.returncode!=0 and 'regenerate the Ninja graph' in child.stderr,child.stderr
 assert not Path('result').exists()
else:
 assert child.returncode==0,child.stderr
 assert Path('result').read_text()==('changed' if mode=='changed' else 'ready')
'''
            (root / "probe.py").write_text(script)
            for mode in ("unchanged", "changed", "corrupt", "unsealed"):
                with self.subTest(mode=mode):
                    context.chmod(0o600)  # Fault injection into a sealed object.
                    context.write_bytes(original)
                    (root / "result").unlink(missing_ok=True)
                    result = subprocess.run(
                        [str(BINARY), "--internal-run-ninja", "", sys.executable,
                         "probe.py", str(BINARY), str(sidecar), str(context), mode, "-j32"],
                        cwd=root, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_new_and_removed_implicit_exports_invalidate_runtime_recipe(self):
        for profile in ("off", "safe", "aggressive"):
            for expression in ("$(shell cat seed)", "$(call $(selected))"):
                with self.subTest(profile=profile, expression=expression), \
                        tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / "seed").write_text("ready")
                    (root / "Makefile").write_text(
                        "selected = reader\nreader = $(shell cat seed)\n"
                        "all: result\nresult: seed\n\t@printf '%s:%s' '" +
                        expression + "' \"$$TEST_EXPORTED_NEW\" > $@\n")
                    env = dict(os.environ)
                    env.pop("TEST_EXPORTED_NEW", None)
                    command = [str(BINARY), "build", "--graph_optimizations=" +
                               profile, "all"]
                    for value in (None, "changed", "updated", None):
                        if value is None:
                            env.pop("TEST_EXPORTED_NEW", None)
                        else:
                            env["TEST_EXPORTED_NEW"] = value
                        built = subprocess.run(command, cwd=root, env=env,
                                               capture_output=True, text=True,
                                               timeout=20)
                        self.assertEqual(built.returncode, 0,
                                         built.stdout + built.stderr)
                        self.assertEqual((root / "result").read_text(),
                                         "ready:" + (value or ""))

    def test_empty_intermediate_and_large_shared_consumer_closure(self):
        cases = [("direct", "safe", 8), ("ninja", "off", 8),
                 ("ninja", "safe", 8), ("ninja", "aggressive", 66000)]
        for mode, profile, count in cases:
            with self.subTest(mode=mode, profile=profile), \
                    tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "leaf").write_text("source")
                (root / "leaf2").write_text("source")
                headers = ["h" + str(i) for i in range(count)]
                for i, name in enumerate(headers):
                    os.link(root / ("leaf" if i % 2 else "leaf2"), root / name)
                makefile = root / "Makefile"
                makefile.write_text(
                    ".INTERMEDIATE: middle\nselected = reader\n"
                    "reader = $(shell cat middle)\nall: bundle\n"
                    "bundle: final middle\n\t@cp final $@\n"
                    "final: middle " + " ".join(headers) +
                    "\n\t@printf %s '$(call $(selected))' > $@\n"
                    "middle:\n\t@printf ready > $@\n")
                command = [str(BINARY)] + (["build"] if mode == "ninja" else [])
                command += ["--graph_optimizations=" + profile, "all"]
                for repeat in range(3):
                    if repeat:
                        with makefile.open("a") as f:
                            f.write("UNRELATED = " + str(repeat) + "\n")
                    built = subprocess.run(command, cwd=root, capture_output=True,
                                           text=True, timeout=120)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertEqual((root / "bundle").read_text(), "ready")
                    self.assertFalse((root / "middle").exists())
                    if repeat == 0:
                        stamps = [(root / n).stat().st_mtime_ns
                                  for n in ("final", "bundle")]
                    else:
                        self.assertEqual([(root / n).stat().st_mtime_ns
                                          for n in ("final", "bundle")], stamps)
                time.sleep(0.01)
                (root / "leaf").write_text("updated")
                (root / "leaf2").write_text("updated")
                built = subprocess.run(command, cwd=root, capture_output=True,
                                       text=True, timeout=120)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                self.assertNotEqual((root / "bundle").stat().st_mtime_ns, stamps[1])

    def test_failed_bootstrap_does_not_publish_observed_inputs(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                (root / "Makefile").write_text(
                    ".DELETE_ON_ERROR:\n-include generated.mk\n"
                    "selected = reader\nreader = $(shell cat seed)\n"
                    "all: result\nresult: generated.mk\n\t@printf %s $(VALUE) > $@\n"
                    "generated.mk: seed\n\t@printf 'VALUE := %s\\n' "
                    "'$(call $(selected))' > $@; test -f permit\n")
                command = [str(BINARY), "build", "--graph_optimizations=" +
                           profile, "all"]
                failed = subprocess.run(command, cwd=root, capture_output=True,
                                        text=True, timeout=20)
                self.assertNotEqual(failed.returncode, 0)
                self.assertFalse(list((root / ".openmktr").glob(".kati_runtime-reads-*")))
                self.assertFalse(list((root / ".openmktr").glob(".kati_bootstrap-*.success")))
                (root / "permit").touch()
                for repeat in range(2):
                    built = subprocess.run(command, cwd=root, capture_output=True,
                                           text=True, timeout=20)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertEqual((root / "result").read_text(), "ready")
                    if repeat == 0:
                        stamp = (root / "generated.mk").stat().st_mtime_ns
                    else:
                        self.assertEqual((root / "generated.mk").stat().st_mtime_ns,
                                         stamp)

    def test_bootstrap_oneshell_exports_and_runtime_assignments(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                makefile = root / "Makefile"
                makefile.write_text(
                    ".ONESHELL:\n-include generated.mk\n"
                    "selected = reader\nreader = $(shell cat seed)\n"
                    "all: result\nresult: generated.mk\n\t@printf %s $(VALUE) > $@\n"
                    "generated.mk: export TOKEN = target\ngenerated.mk: seed\n"
                    "\t@$(eval LOCAL := expanded)\n"
                    "\ttest \"$$TOKEN\" = target\n"
                    "\tprintf 'VALUE := %s-%s\\n' '$(call $(selected))' "
                    "'$(LOCAL)' > $@\n")
                command = [str(BINARY), "build", "--graph_optimizations=" +
                           profile, "all"]
                for repeat in range(3):
                    if repeat:
                        with makefile.open("a") as f:
                            f.write("UNRELATED = " + str(repeat) + "\n")
                    built = subprocess.run(command, cwd=root, capture_output=True,
                                           text=True, timeout=20)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertEqual((root / "result").read_text(), "ready-expanded")
                    if repeat == 0:
                        stamp = (root / "generated.mk").stat().st_mtime_ns
                    else:
                        self.assertEqual((root / "generated.mk").stat().st_mtime_ns,
                                         stamp)

    def test_context_regeneration_preserves_missing_intermediate_consumers(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                makefile = root / "Makefile"
                makefile.write_text(
                    ".INTERMEDIATE: middle\nselected = reader\n"
                    "reader = $(shell cat middle)\nall: result\n"
                    "result: middle\n\t@printf %s '$(call $(selected))' > $@\n"
                    "middle: seed\n\t@cp $< $@\n")
                command = [str(BINARY), "build", "--graph_optimizations=" +
                           profile, "all"]
                for repeat in range(3):
                    if repeat:
                        with makefile.open("a") as f:
                            f.write("UNRELATED = " + str(repeat) + "\n")
                    built = subprocess.run(command, cwd=root, capture_output=True,
                                           text=True, timeout=20)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertEqual((root / "result").read_text(), "ready")
                    self.assertFalse((root / "middle").exists())
                    if repeat == 0:
                        stamp = (root / "result").stat().st_mtime_ns
                    else:
                        self.assertEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_missing_intermediate_with_large_consumers_and_order_only_graph(self):
        for profile in ("off", "safe", "aggressive"):
            for shared in (False, True):
                with self.subTest(profile=profile, shared=shared), \
                        tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / "seed").write_text("ready")
                    headers = ["h" + str(i) for i in range(2048)]
                    for name in headers:
                        (root / name).touch()
                    prerequisites = " ".join(headers)
                    (root / "Makefile").write_text(
                        ".INTERMEDIATE: middle\nall: final\n"
                        "final: middle " + (prerequisites if shared else "") +
                        " | library\n\t@cp middle $@\n"
                        "middle: seed\n\t@cp $< $@\n"
                        "library: " + prerequisites + "\n\t@touch $@\n")
                    command = [str(BINARY), "build", "--graph_optimizations=" +
                               profile, "all"]
                    for repeat in range(3):
                        built = subprocess.run(command, cwd=root, capture_output=True,
                                               text=True, timeout=30)
                        self.assertEqual(built.returncode, 0,
                                         built.stdout + built.stderr)
                        self.assertFalse((root / "middle").exists())
                        if repeat == 0:
                            stamp = (root / "final").stat().st_mtime_ns
                        else:
                            self.assertEqual((root / "final").stat().st_mtime_ns, stamp)
                    (root / "seed").write_text("changed")
                    built = subprocess.run(command, cwd=root, capture_output=True,
                                           text=True, timeout=30)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertEqual((root / "final").read_text(), "changed")

    def test_bootstrap_runtime_output_retains_successful_read_certificate(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                (root / "Makefile").write_text(
                    "-include generated.mk\nselected = reader\n"
                    "reader = $(shell cat seed)\nall: result tool\n"
                    "generated.mk: tool\n\t@./tool | sed 's/^/VALUE := /' > $@\n"
                    "tool: seed\n\t@printf '%s\\n' '#!/bin/sh' "
                    "'printf %s\\\\n $(call $(selected))' > $@; "
                    "chmod +x $@; printf 'run\\n' >> calls\n"
                    "result: seed\n\t@./tool > $@\n")
                command = [str(BINARY), "build", "--graph_optimizations=" +
                           profile, "all"]
                for repeat in range(3):
                    if repeat == 1:
                        with (root / "Makefile").open("a") as f:
                            f.write("UNRELATED = changed\n")
                    built = subprocess.run(command, cwd=root, capture_output=True,
                                           text=True, timeout=20)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertEqual((root / "calls").read_text(), "run\n",
                                     (repeat, built.stdout, built.stderr))
                    if repeat == 0:
                        stamp = (root / "result").stat().st_mtime_ns
                    else:
                        self.assertEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_runtime_diagnostics_preserve_caller_location_and_stream(self):
        payload = "-n -e 100% 'quoted' $HOME \\literal\nnext\tline"
        for profile in ("off", "safe", "aggressive"):
            for function in ("warning", "error"):
                with self.subTest(profile=profile, function=function), \
                        tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / "seed").touch()
                    (root / "Makefile").write_text(
                        "define TEXT\n" + payload.replace("$", "$$") +
                        "\nendef\nselected = reader\nreader = "
                        "$(if $(wildcard seed),$(" + function + " $(TEXT)))\n"
                        "all: result\nresult: seed\n"
                        "\t@$(call $(selected))printf done > $@\n")
                    direct = subprocess.run([str(BINARY), "all"], cwd=root,
                                            capture_output=True, text=True, timeout=20)
                    (root / "result").unlink(missing_ok=True)
                    generated = subprocess.run(
                        [str(BINARY), "--ninja", "--graph_optimizations=" + profile, "all"],
                        cwd=root, capture_output=True, text=True, timeout=20)
                    self.assertEqual(generated.returncode, 0,
                                     generated.stdout + generated.stderr)
                    command = subprocess.run(["ninja", "-t", "commands", "result"],
                                             cwd=root, capture_output=True,
                                             text=True, timeout=20)
                    self.assertEqual(command.returncode, 0, command.stderr)
                    emitted = subprocess.run(["sh", "-c", command.stdout], cwd=root,
                                             capture_output=True, text=True, timeout=20)
                    self.assertIn("Makefile:", direct.stderr)
                    self.assertEqual(direct.stderr, emitted.stderr)
                    self.assertIn(payload, emitted.stderr)
                    self.assertNotIn(payload, emitted.stdout)
                    self.assertEqual(emitted.returncode == 0, function == "warning")

    def test_make_flags_are_stable_after_generated_include_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "Makefile").write_text(
                "-include generated.mk\nselected = reader\n"
                "reader = $(origin MFLAGS):$(flavor MFLAGS):$(MFLAGS):$(shell cat seed)\n"
                "all: result\ngenerated.mk: seed\n\t@printf 'EXTRA := ready\\n' > $@\n"
                "result: seed\n\t@printf '%s' '$(call $(selected))' > $@\n")
            env = dict(os.environ)
            env.pop("MFLAGS", None)
            command = [str(BINARY), "build", "-rs", "-j2", "all"]
            for repeat in range(3):
                built = subprocess.run(command, cwd=root, env=env, capture_output=True,
                                       text=True, timeout=20)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                self.assertIn("environment:recursive:", (root / "result").read_text())
                self.assertIn("-j2", (root / "result").read_text())
                if repeat == 0:
                    stamp = (root / "result").stat().st_mtime_ns
                else:
                    self.assertEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_static_script_guard_tracks_relevant_recipe_context(self):
        for size in (3, 8000):
            for profile in ("off", "safe", "aggressive"):
                with self.subTest(size=size, profile=profile), \
                        tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / "seed").touch()
                    (root / "vars.mk").write_text("WORD = " + "a" * size + "\n")
                    (root / "Makefile").write_text(
                        "include vars.mk\nall: result\nresult: seed\n"
                        "\t@printf '%s' '$(WORD)' > $@\n")
                    command = [str(BINARY), "build",
                               "--graph_optimizations=" + profile, "all"]
                    for value, unrelated in (("a", False), ("a", True), ("b", False)):
                        (root / "vars.mk").write_text(
                            "WORD = " + value * size + "\n" +
                            ("UNRELATED = changed\n" if unrelated else ""))
                        built = subprocess.run(command, cwd=root, capture_output=True,
                                               text=True, timeout=20)
                        self.assertEqual(built.returncode, 0,
                                         built.stdout + built.stderr)
                        self.assertEqual((root / "result").read_text(), value * size)
                        if value == "a" and not unrelated:
                            stamp = (root / "result").stat().st_mtime_ns
                        if unrelated:
                            self.assertEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_nested_bare_ninja_does_not_inherit_parent_goals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "Makefile").write_text(
                ".INTERMEDIATE: middle\nall: final nested\n"
                "final: middle\n\t@cp $< $@\n"
                "middle: seed\n\t@cp $< $@\n"
                "nested: final\n\t@ninja -j1 -f .openmktr/build.ninja middle; "
                "cat middle > $@\n")
            built = subprocess.run([str(BINARY), "build", "-j1", "all"],
                                   cwd=root, capture_output=True, text=True, timeout=20)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            self.assertFalse((root / "middle").exists())
            (root / "nested").unlink()
            nested = subprocess.run(["sh", ".openmktr/ninja.sh", "-j1", "nested"],
                                    cwd=root, capture_output=True, text=True, timeout=20)
            self.assertEqual(nested.returncode, 0, nested.stdout + nested.stderr)
            self.assertEqual((root / "nested").read_text(), "ready")

    def test_bounded_environment_reference_regenerates_without_expansion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "Makefile").write_text(
                "unexport UNUSED\nreader = $(origin UNUSED):$(UNUSED):$(shell cat seed)\n"
                "all: result\nresult: seed\n"
                "\t@printf '%s' '$(call reader)' > $@\n")
            for value in (None, "one", "two", None, ""):
                env = dict(os.environ)
                env.pop("UNUSED", None)
                if value is not None:
                    env["UNUSED"] = value
                result = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                        env=env, capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((root / "result").read_text(),
                                 ("undefined" if value is None else "environment") +
                                 ":" + (value or "") + ":ready")

    def test_missing_intermediate_chain_keeps_current_parent(self):
        for profile in ("off", "safe", "aggressive"):
            for mode in ("direct", "ninja"):
                with self.subTest(profile=profile, mode=mode), \
                        tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / "seed").write_text("one")
                    (root / "Makefile").write_text(
                        ".INTERMEDIATE: first second\nall: final\n"
                        "final: second\n\t@cp $< $@\n"
                        "second: first\n\t@cp $< $@\n"
                        "first: seed\n\t@cp $< $@\n")
                    command = [str(BINARY)] + (["build"] if mode == "ninja" else [])
                    command += ["--graph_optimizations=" + profile, "all"]

                    def build():
                        result = subprocess.run(command, cwd=root, capture_output=True,
                                                text=True, timeout=20)
                        self.assertEqual(result.returncode, 0,
                                         result.stdout + result.stderr)

                    build()
                    stamp = (root / "final").stat().st_mtime_ns
                    build()
                    self.assertEqual((root / "final").stat().st_mtime_ns, stamp)
                    self.assertFalse((root / "first").exists())
                    self.assertFalse((root / "second").exists())
                    time.sleep(0.01)
                    (root / "seed").write_text("two")
                    build()
                    self.assertEqual((root / "final").read_text(), "two")
                    self.assertFalse((root / "first").exists())
                    self.assertFalse((root / "second").exists())
                    if mode == "ninja":
                        makefile = root / "Makefile"
                        makefile.write_text(makefile.read_text().replace(
                            "final: second\n\t@cp $< $@",
                            "final: second\n\t@printf 'recipe:' > $@; cat $< >> $@"))
                        build()
                        self.assertEqual((root / "final").read_text(), "recipe:two")
                        explicit = subprocess.run(["sh", ".openmktr/ninja.sh", "second"],
                                                  cwd=root, capture_output=True,
                                                  text=True, timeout=20)
                        self.assertEqual(explicit.returncode, 0,
                                         explicit.stdout + explicit.stderr)
                        self.assertEqual((root / "second").read_text(), "two")

    def test_unexported_environment_only_invalidates_actual_reads(self):
        for read in (False, True):
            with self.subTest(read=read), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                (root / "Makefile").write_text(
                    "unexport UNUSED\nselected = reader\nreader = " +
                    ("$(UNUSED):" if read else "") + "$(shell cat seed)\n"
                    "all: result\nresult: seed\n"
                    "\t@printf '%s' '$(call $(selected))' > $@\n")
                for value in ("one", "two", None):
                    env = dict(os.environ)
                    env.pop("UNUSED", None)
                    if value is not None:
                        env["UNUSED"] = value
                    result = subprocess.run([str(BINARY), "build", "all"],
                                            cwd=root, env=env, capture_output=True,
                                            text=True, timeout=20)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)
                    self.assertEqual((root / "result").read_text(),
                                     ((value or "") + ":" if read else "") + "ready")
                    if value == "one":
                        stamp = (root / "result").stat().st_mtime_ns
                    elif not read:
                        self.assertEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_warning_and_error_diagnostics_preserve_stream_and_data(self):
        payload = "-n -e 100% 'quoted' $HOME \\literal\nnext\tline"
        for function in ("warning", "error"):
            with self.subTest(function=function), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").touch()
                (root / "Makefile").write_text(
                    "define TEXT\n" + payload.replace("$", "$$") +
                    "\nendef\nall: result\nresult: seed\n"
                    "\t@$(" + function + " $(TEXT))printf done > $@\n")
                direct = subprocess.run([str(BINARY), "all"], cwd=root,
                                        capture_output=True, text=True, timeout=20)
                if (root / "result").exists():
                    (root / "result").unlink()
                generated = subprocess.run([str(BINARY), "--ninja", "all"],
                                           cwd=root, capture_output=True,
                                           text=True, timeout=20)
                self.assertEqual(generated.returncode, 0,
                                 generated.stdout + generated.stderr)
                command = subprocess.run(["ninja", "-t", "commands", "result"],
                                         cwd=root, capture_output=True,
                                         text=True, timeout=20)
                self.assertEqual(command.returncode, 0, command.stderr)
                # Execute the emitted command directly to observe its streams;
                # Ninja normally combines a recipe's stdout and stderr.
                emitted = subprocess.run(["sh", "-c", command.stdout], cwd=root,
                                         capture_output=True, text=True, timeout=20)
                self.assertEqual(direct.stderr, emitted.stderr)
                self.assertIn(payload, emitted.stderr)
                self.assertNotIn(payload, emitted.stdout)
                self.assertEqual(direct.returncode == 0, function == "warning")
                self.assertEqual(emitted.returncode == 0, function == "warning")

    def test_removed_path_and_order_only_symlink_observations(self):
        for profile in ("off", "safe", "aggressive"):
            for mode in ("direct", "ninja"):
                for symlink in (False, True):
                    with self.subTest(profile=profile, mode=mode, symlink=symlink), \
                            tempfile.TemporaryDirectory() as tmp:
                        root = Path(tmp)
                        if symlink:
                            (root / "old").touch()
                            (root / "new").touch()
                            (root / "observed").symlink_to("old")
                            producer = "ln -sf new observed"
                            expression = "$(notdir $(realpath observed))"
                            prerequisite = "| producer"
                            expected = "new"
                        else:
                            (root / "observed").touch()
                            producer = "rm observed"
                            expression = "$(if $(wildcard observed),present,missing)"
                            prerequisite = "producer"
                            expected = "missing"
                        (root / "Makefile").write_text(
                            "all: result\nproducer:\n\t@" + producer +
                            "; touch $@\nresult: " + prerequisite +
                            "\n\t@printf '%s' '" + expression + "' > $@\n")
                        command = [str(BINARY)] + (["build"] if mode == "ninja" else [])
                        command += ["--graph_optimizations=" + profile, "all"]
                        result = subprocess.run(command, cwd=root, capture_output=True,
                                                text=True, timeout=20)
                        self.assertEqual(result.returncode, 0,
                                         result.stdout + result.stderr)
                        self.assertEqual((root / "result").read_text(), expected)

    def test_unavailable_observation_cache_preserves_build_and_invalidation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "vars.mk").write_text("WORD = one\n")
            (root / "Makefile").write_text(
                "include vars.mk\nselected = reader\n"
                "reader = $(WORD):$(shell cat seed)\n"
                "all: result\nresult: seed\n"
                "\t@printf '%s' '$(call $(selected))' > $@\n")
            generated = subprocess.run(
                [str(BINARY), "--ninja", "--regen", "--ninja_dir", ".openmktr", "all"],
                cwd=root, capture_output=True, text=True, timeout=20)
            self.assertEqual(generated.returncode, 0,
                             generated.stdout + generated.stderr)
            states = {path.with_name(path.name.split(".version-", 1)[0])
                      for path in (root / ".openmktr/.kati_recipes").iterdir()}
            self.assertTrue(states)
            for path in states:
                Path(str(path) + ".reads-pending").mkdir()
            built = subprocess.run(["sh", ".openmktr/ninja.sh", "-j2"], cwd=root,
                                   capture_output=True, text=True, timeout=20)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            self.assertEqual((root / "result").read_text(), "one:ready")
            (root / "vars.mk").write_text("WORD = two\n")
            updated = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                     capture_output=True, text=True, timeout=20)
            self.assertEqual(updated.returncode, 0, updated.stdout + updated.stderr)
            self.assertEqual((root / "result").read_text(), "two:ready")

    def test_lost_observation_cannot_reuse_an_older_context_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("a")
            (root / "vars.mk").write_text("A = one\nB = two\n")
            (root / "Makefile").write_text(
                "include vars.mk\nselected = reader\n"
                "reader = $(if $(filter a,$(shell cat seed)),$(A),$(B))\n"
                "all: result\nresult: seed\n"
                "\t@printf '%s' '$(call $(selected))' > $@\n")

            def build(expected):
                result = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((root / "result").read_text(), expected)

            build("one")
            time.sleep(0.01)
            (root / "seed").write_text("b")
            (root / "vars.mk").write_text("A = one\nB = changed\n")
            build("changed")
            observations = list((root / ".openmktr").glob(".kati_runtime-reads-*"))
            self.assertTrue(observations)
            for path in observations:
                path.unlink()
            (root / "vars.mk").write_text("A = one\nB = two\n")
            build("two")

    def test_failed_recipe_does_not_publish_observed_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "vars.mk").write_text("WORD = one\nMODE = ok\n")
            (root / "Makefile").write_text(
                ".DELETE_ON_ERROR:\ninclude vars.mk\nselected = reader\n"
                "reader = $(WORD):$(shell cat seed)\n"
                "all: result\nresult: seed\n"
                "\t@printf '%s' '$(call $(selected))' > $@; "
                "$(if $(filter fail,$(MODE)),false,true)\n")
            command = [str(BINARY), "build", "all"]
            first = subprocess.run(command, cwd=root, capture_output=True,
                                   text=True, timeout=20)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            paths = list((root / ".openmktr").glob(".kati_runtime-reads-*"))
            self.assertTrue(paths)
            observations = {p: p.read_bytes() for p in paths}
            (root / "vars.mk").write_text("WORD = two\nMODE = fail\n")
            failed = subprocess.run(command, cwd=root, capture_output=True,
                                    text=True, timeout=20)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse((root / "result").exists())
            self.assertTrue(all(not p.exists() for p in paths))
            (root / "vars.mk").write_text("WORD = two\nMODE = ok\n")
            recovered = subprocess.run(command, cwd=root, capture_output=True,
                                       text=True, timeout=20)
            self.assertEqual(recovered.returncode, 0,
                             recovered.stdout + recovered.stderr)
            self.assertEqual((root / "result").read_text(), "two:ready")
            self.assertNotEqual({p: p.read_bytes() for p in paths}, observations)

    def test_computed_context_reuses_only_successful_observed_inputs(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                (root / "header").touch()
                (root / "vars.mk").write_text("WORD = one\n")
                (root / "Makefile").write_text(
                    "include vars.mk\n-include $(wildcard *.dep)\n"
                    "selected = reader\nreader = $(WORD):$(shell cat seed)\n"
                    "all: result\nresult: seed\n"
                    "\t@printf '%s' '$(call $(selected))' > $@\n")
                command = [str(BINARY), "build",
                           "--graph_optimizations=" + profile, "all"]

                def build():
                    result = subprocess.run(command, cwd=root, capture_output=True,
                                            text=True, timeout=20)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

                build()
                stamp = (root / "result").stat().st_mtime_ns
                (root / "headers.dep").write_text("result: header\n")
                (root / "vars.mk").write_text("WORD = one\nUNRELATED = changed\n")
                build()
                self.assertEqual((root / "result").stat().st_mtime_ns, stamp)
                for word in ("two", "one"):
                    (root / "vars.mk").write_text("WORD = " + word + "\n")
                    build()
                    self.assertEqual((root / "result").read_text(), word + ":ready")

    def test_large_script_guard_settles_without_touching_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").touch()
            (root / "result").write_text("current")
            stamp = (root / "result").stat().st_mtime_ns
            (root / "Makefile").write_text(
                "all: result\nresult: seed\n\t@printf '%s' '" +
                "x" * 8000 + "' > $@\n")
            for invocation in range(2):
                result = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)
                self.assertEqual((root / "result").stat().st_mtime_ns, stamp)
                if invocation:
                    self.assertIn("no work to do", result.stdout)
            time.sleep(0.01)
            (root / "seed").write_text("changed")
            result = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((root / "result").read_text(), "x" * 8000)

    def test_large_runtime_guard_with_multiple_sidecars(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                names = ["header_name_" + str(i).zfill(4) for i in range(512)]
                for name in names:
                    (root / name).touch()
                (root / "seed").write_text("ready")
                (root / "result").write_text("current")
                stamp = (root / "result").stat().st_mtime_ns
                (root / "Makefile").write_text(
                    "all: result\nresult: seed " + " ".join(names) + "\n"
                    "\t@printf '%s' '$(shell cat seed)' > $@\n")
                command = [str(BINARY), "build",
                           "--graph_optimizations=" + profile, "all"]
                for invocation in range(2):
                    result = subprocess.run(command, cwd=root,
                                            capture_output=True, text=True,
                                            timeout=20)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)
                    self.assertEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_old_depfile_addition_does_not_change_unused_automatic_inputs(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                (root / "header").touch()
                (root / "deps.mk").write_text("")
                (root / "Makefile").write_text(
                    "include deps.mk\nall: result\nresult: seed\n"
                    "\t@printf '%s' '$(shell cat seed)' > $@\n")
                command = [str(BINARY), "build",
                           "--graph_optimizations=" + profile, "all"]
                first = subprocess.run(command, cwd=root, capture_output=True,
                                       text=True, timeout=20)
                self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
                stamp = (root / "result").stat().st_mtime_ns
                (root / "deps.mk").write_text("result: header\n")
                second = subprocess.run(command, cwd=root, capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
                self.assertEqual((root / "result").stat().st_mtime_ns, stamp)
                time.sleep(0.01)
                (root / "header").write_text("changed")
                third = subprocess.run(command, cwd=root, capture_output=True,
                                       text=True, timeout=20)
                self.assertEqual(third.returncode, 0, third.stdout + third.stderr)
                self.assertNotEqual((root / "result").stat().st_mtime_ns, stamp)

    def test_pure_foreach_avoids_unrelated_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").touch()
            (root / "Makefile").write_text(
                "UNRELATED := " + "x" * 100000 + "\nall: result\n"
                "result: seed\n\t@printf '%s' '$(foreach n,one two,$(n))' > $@\n")
            result = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((root / "result").read_text(), "one two")
            self.assertLess(sum(p.stat().st_size for p in root.rglob(
                ".kati_runtime-context-*")), 4096)

    def test_runtime_failure_cleanup(self):
        for precious in (False, True):
            with self.subTest(precious=precious), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("partial")
                (root / "Makefile").write_text(
                    (".PRECIOUS: result\n" if precious else "") +
                    ".DELETE_ON_ERROR:\nall: result\nresult: seed\n"
                    "\t@printf '%s' '$(shell cat seed)' > $@; exit 1\n")
                result = subprocess.run([str(BINARY), "build", "all"], cwd=root,
                                        capture_output=True, text=True, timeout=20)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual((root / "result").exists(), precious)

    def test_deferred_info_preserves_literal_data(self):
        expected = "-e 100% 'quoted' $HOME \\literal\nnext\tline"
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").touch()
                (root / "Makefile").write_text(
                    "define TEXT\n" + expected.replace("$", "$$") +
                    "\nendef\nall: result\nresult: seed\n"
                    "\t@$(info $(TEXT))printf done > $@\n")
                command = [str(BINARY), "all"] if mode == "direct" else [
                    str(BINARY), "build", "all"]
                result = subprocess.run(command, cwd=root, capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)
                self.assertIn(expected + "\n", result.stdout)

    def test_runtime_consumers_overlap_within_job_limit(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "Makefile").write_text(
                    "all: a b\ngenerated:\n\t@printf ready > $@\n"
                    "a b: generated\n"
                    "\t@python3 worker.py '$(strip $(shell cat generated))' $@\n")
                (root / "worker.py").write_text(
                    "import fcntl, json, pathlib, sys, time\n"
                    "assert sys.argv[1] == 'ready'\n"
                    "def change(delta):\n"
                    " with open('counter', 'a+') as f:\n"
                    "  fcntl.flock(f, fcntl.LOCK_EX); f.seek(0)\n"
                    "  state=json.loads(f.read() or '[0,0,0]')\n"
                    "  state[0]+=delta; state[1]=max(state[0],state[1])\n"
                    "  state[2]+=delta>0\n"
                    "  f.seek(0); f.truncate(); json.dump(state,f); f.flush()\n"
                    "  return state\n"
                    "change(1)\n"
                    "deadline=time.monotonic()+5\n"
                    "while change(0)[2] < 2:\n"
                    " assert time.monotonic()<deadline, 'consumers serialized'\n"
                    " time.sleep(.01)\n"
                    "change(-1); pathlib.Path(sys.argv[2]).touch()\n")
                for command in ([str(BINARY), "--ninja", "--regen",
                                 "--graph_optimizations=" + profile, "all"],
                                ["sh", "./ninja.sh", "-j2"]):
                    result = subprocess.run(command, cwd=root, capture_output=True,
                                            text=True, timeout=15)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)
                import json
                self.assertEqual(json.loads((root / "counter").read_text()),
                                 [0, 2, 2])

    def run_case(self, expression, expected, mode, profile, prefix=""):
        with tempfile.TemporaryDirectory(prefix="omktr-generated-read-") as tmp:
            root = Path(tmp)
            (root / "seed").write_text("dir/b dir/a other/b\n")
            (root / "Makefile").write_text(
                prefix + "transform = $(sort $(notdir $(1)))\n"
                "reader = $(shell cat $(1))\n"
                "selected = reader\n"
                "all: result\n"
                "generated: seed\n\t@cp $< $@\n"
                "result: generated\n"
                "\t@printf '%s' '" + expression + "' > $@\n")

            def run(*args):
                result = subprocess.run(
                    args, cwd=root, capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)
                return result.stdout + result.stderr

            flags = ("--graph_optimizations=" + profile,)
            if mode == "ninja":
                output = run(str(BINARY), "--ninja", "--regen", *flags, "all")
                self.assertFalse((root / "generated").exists())
                self.assertFalse((root / "effect").exists())
                run("sh", "./ninja.sh", "-j2")
            else:
                run(str(BINARY), *flags, "all")
            self.assertEqual((root / "result").read_text(), expected)
            if mode == "ninja":
                self.assertNotIn("No such file", output)
                stamp = (root / "result").stat().st_mtime_ns
                run("sh", "./ninja.sh", "-j2")
                self.assertEqual((root / "result").stat().st_mtime_ns, stamp)
                sidecars = list(root.glob(".kati_runtime-*"))
                self.assertTrue(sidecars)
                for sidecar in sidecars:
                    sidecar.chmod(0o600)  # Deliberately break an immutable object.
                    sidecar.write_bytes(b"corrupt")
                (root / "result").unlink()
                failed = subprocess.run(
                    ["sh", "./ninja.sh", "-j2"], cwd=root,
                    capture_output=True, text=True, timeout=15)
                self.assertNotEqual(failed.returncode, 0)
                self.assertIn("regenerate", failed.stdout + failed.stderr)

    def check_expression(self, expression, expected, prefix=""):
        for profile in ("off", "safe", "aggressive"):
            for mode in ("direct", "ninja"):
                with self.subTest(profile=profile, mode=mode):
                    self.run_case(expression, expected, mode, profile, prefix)

    def test_quoted_component_contents(self):
        self.check_expression("$(shell cat generated)", "dir/b dir/a other/b")

    def test_runtime_automatic_prerequisite_lists(self):
        self.check_expression("$+:$^:$<:$|:$(shell cat generated)",
                              "generated:generated:generated::dir/b dir/a other/b")

    def test_wildcard_observes_completed_producer(self):
        self.check_expression("$(if $(wildcard generated),ready,missing)",
                              "ready")

    def test_realpath_observes_completed_producer(self):
        self.check_expression("$(if $(realpath generated),ready,missing)",
                              "ready")

    def test_realpath_of_missing_path_is_empty(self):
        self.check_expression("$(if $(realpath absent),present,missing)",
                              "missing")

    def test_dense_shared_sources_preserve_automatic_variables(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                names = ["header_" + str(i).zfill(4) for i in range(256)]
                for name in names:
                    (root / name).write_text("header")
                inputs = " ".join(names + [names[0]])
                unique = " ".join(names)
                (root / "Makefile").write_text(
                    "all: a b\na b: " + inputs + "\n"
                    "\t@printf '%s' '$+|$^' > $@\n" +
                    "".join(name + ":\n" for name in names))
                command = [str(BINARY), "build",
                           "--graph_optimizations=" + profile, "all"]
                for invocation in range(2):
                    result = subprocess.run(command, cwd=root,
                                            capture_output=True, text=True,
                                            timeout=20)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)
                    for name in ("a", "b"):
                        self.assertEqual((root / name).read_text(),
                                         inputs + "|" + unique)

    def test_runtime_context_invalidation(self):
        for profile in ("off", "safe", "aggressive"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "seed").write_text("ready")
                (root / "Makefile").write_text(
                    "include vars.mk\nall: result\nresult: seed\n"
                    "\t@printf '%s' '$(WORD):$(shell cat seed)' > $@\n")
                command = [str(BINARY), "build",
                           "--graph_optimizations=" + profile, "all"]

                def build(word, unrelated):
                    (root / "vars.mk").write_text(
                        "WORD = " + word + "\nUNRELATED = " + unrelated + "\n")
                    result = subprocess.run(command, cwd=root,
                                            capture_output=True, text=True,
                                            timeout=20)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

                build("first", "one")
                original = (root / "result").stat().st_mtime_ns
                build("first", "two")
                self.assertEqual((root / "result").stat().st_mtime_ns, original)
                build("second", "two")
                self.assertEqual((root / "result").read_text(), "second:ready")
                settled = (root / "result").stat().st_mtime_ns
                result = subprocess.run(command, cwd=root, capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)
                self.assertEqual((root / "result").stat().st_mtime_ns, settled)

    def test_unused_runtime_expression_and_context_corruption_are_rejected(self):
        import struct

        def pack(fields):
            return b"".join(struct.pack("<Q", len(x)) + x for x in fields)

        def parts(data):
            result = []
            while data:
                length = struct.unpack_from("<Q", data)[0]
                result.append(data[8:8 + length])
                data = data[8 + length:]
            return result

        def cache(payload):
            checksum = 14695981039346656037
            for byte in payload:
                checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
            return pack([b"OMKTR_GRAPH1", str(checksum).encode(), payload])

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "seed").write_text("ready")
            (root / "Makefile").write_text(
                "selected = reader\nreader = $(shell cat seed)\n"
                "UNUSED = $(never_used)\nall: result\nresult: seed\n"
                "\t@printf '%s' '$(call $(selected))' > $@\n")
            subprocess.run([str(BINARY), "--ninja", "-j32", "all"], cwd=root,
                           capture_output=True, check=True, timeout=30)
            sidecar = next(p for p in root.glob(".kati_runtime-*")
                           if not p.name.startswith((".kati_runtime-context-",
                                                     ".kati_runtime-reads-")))
            context = root / parts(parts(sidecar.read_bytes())[2])[7].decode()
            original = context.read_bytes()
            definitions = parts(parts(original)[2])
            for i, definition in enumerate(definitions):
                fields = parts(definition)
                if fields[0] == b"UNUSED":
                    fields[3] = pack([b"unknown-expression-kind"])
                    definitions[i] = pack(fields)
                    break
            else:
                self.fail("Unused definition missing from full runtime context")
            context.chmod(0o600)  # Fault injection into a sealed object.
            for broken in (original[:-1], original + b"trailing",
                           b"\xff" * 8 + original[8:], cache(pack(definitions))):
                context.write_bytes(broken)
                result = subprocess.run([str(BINARY), "--internal-evaluate-recipe",
                                         str(sidecar)], cwd=root, capture_output=True,
                                        text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("regenerate the Ninja graph", result.stderr)
                self.assertFalse((root / "result").exists())
            context.write_bytes(original)
            subprocess.run([str(BINARY), "--internal-evaluate-recipe", str(sidecar)],
                           cwd=root, capture_output=True, check=True, timeout=30)
            self.assertEqual((root / "result").read_text(), "ready")

    def test_lazy_runtime_symbols_and_recursive_values(self):
        definitions = "".join(f"UNUSED{i} = $(missing{i})tail{i}\n" for i in range(3000))
        self.check_expression(
            "$(words $(filter UNUSED%,$(.VARIABLES))):"
            "$(words $(filter UNUSED%,$(.KATI_SYMBOLS))):"
            "$(flavor UNUSED1):$(origin UNUSED1):$(UNUSED1):"
            "$(call transform,$(shell cat generated))",
            "3000:3000:recursive:file:tail1:a b", definitions)

    def test_installer_transform(self):
        self.check_expression(
            "$(call transform,$(shell cat generated))", "a b")

    def test_foreach_directory_transform(self):
        self.check_expression(
            "$(sort $(foreach entry,$(shell cat generated),$(dir $(entry))))",
            "dir/ other/")

    def test_computed_macro_read(self):
        self.check_expression(
            "$(call transform,$(call $(selected),generated))", "a b")

    def test_recipe_effects_and_automatic_variables(self):
        self.check_expression(
            "$(eval LOCAL := ready)$(file >effect,$(LOCAL))"
            "$(LOCAL):$@:$<:$(call transform,$(shell cat generated))",
            "ready:result:generated:a b")

    def test_oneshell_target_specific_context(self):
        self.check_expression(
            "$(flavor WORD):$(origin WORD):$(WORD):"
            "$(call transform,$(shell cat generated))",
            "simple:file:target:a b",
            ".ONESHELL:\nresult: WORD := target\n")

    def test_target_export_reaches_recipe_process(self):
        self.check_expression(
            "$(call transform,$(shell cat generated))'\"$$WORD\"'",
            "a bresult", "result: export WORD = $@\n")


if __name__ == "__main__":
    unittest.main()
