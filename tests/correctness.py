#!/usr/bin/env python3
"""Behavioral checks for build correctness across direct and Ninja modes."""

import json
import os
import signal
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


KATI = Path(os.environ.get(
    "OMKTR_BINARY", Path(__file__).resolve().parents[1] / "omktr"
)).resolve()


class BuildCorrectness(unittest.TestCase):
    def test_background_recipes_remain_valid_inside_freshness_guards(self):
        for mode in ("direct", "ninja"):
            for existing in (False, True):
                with self.subTest(mode=mode, existing=existing):
                    output = self.directory / "output"
                    output.unlink(missing_ok=True)
                    (self.directory / "input").write_text("source")
                    prerequisites = "input" if existing else ""
                    self.makefile(f"all: output\noutput: {prerequisites}\n"
                                  "\t@printf hello > output &\n")
                    if existing:
                        output.write_text("old")
                        os.utime(output, ns=(100, 100))
                    if mode == "direct":
                        self.direct()
                    else:
                        self.ninja()
                    self.assertEqual(output.read_text(), "hello")

    def test_trailing_literal_ampersands_remain_recipe_data(self):
        for value in ("\\&", "'&'", '"&"'):
            with self.subTest(value=value):
                (self.directory / "output").unlink(missing_ok=True)
                self.makefile(f"all: output\noutput:\n\t@> output printf '%s' {value}\n")
                self.ninja()
                self.assertEqual((self.directory / "output").read_text(), "&")

    def test_saved_compiler_command_retains_literal_hash_escape(self):
        self.makefile(
            "pound := \\#\nsquote := '\n"
            "quote = $(subst $(squote),'\\$(squote)',$1)\n"
            "compile = printf \"value=\\#hash\" > output\n"
            "saved = $(call quote,$(subst $(pound),$$(pound),$(compile)))\n"
            "difference = $(filter-out $(compile),$(previous)) "
            "$(filter-out $(previous),$(compile))\n"
            "-include output.cmd\n.PHONY: all force\nall: output\n"
            "output: force\n\t@$(if $(strip $(difference)),"
            "$(compile); printf x >> runs; "
            "printf '%s\\n' 'previous := $(saved)' > output.cmd,:)\n")
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode):
                for name in ("output", "output.cmd", "runs"):
                    (self.directory / name).unlink(missing_ok=True)
                for _ in range(3):
                    if mode == "direct":
                        self.direct("all")
                    else:
                        self.direct("build", "-j2", "all")
                self.assertEqual((self.directory / "output").read_text(), "value=#hash")
                self.assertIn("$(pound)", (self.directory / "output.cmd").read_text())
                self.assertEqual((self.directory / "runs").read_text(), "x")

    def test_deferred_recursive_compiler_assignment_still_expands(self):
        (self.directory / "child.mk").write_text(
            "all:\n\t@$(TOOL) '%s' done > result\n")
        for assignment in ("'TOOL=$$(RAW_TOOL)'", "TOOL='$$(RAW_TOOL)'"):
            for mode in ("direct", "ninja"):
                with self.subTest(assignment=assignment, mode=mode):
                    self.makefile("RAW_TOOL := printf\nall:\n"
                                  f"\t@$(MAKE) -f child.mk {assignment}\n")
                    if mode == "direct":
                        self.direct("all")
                    else:
                        self.direct("build", "-j2", "all")
                    self.assertEqual((self.directory / "result").read_text(), "done")

    def test_deferred_recursive_assignment_operators_preserve_values(self):
        # These cases start with an undefined child variable. Portage exports
        # CFLAGS, which += must retain and ?= must leave unchanged.
        inherited_cflags = os.environ.pop("CFLAGS", None)
        if inherited_cflags is not None:
            self.addCleanup(os.environ.__setitem__, "CFLAGS", inherited_cflags)
        (self.directory / "child.mk").write_text(
            "all:\n\t@printf '%s' '$(CFLAGS)' > result\n")
        for operator in ("=", "+=", ":=", "?="):
            for quoted in ("'CFLAGS{op}$$(EXTRA_FLAGS)'",
                           "CFLAGS{op}'$$(EXTRA_FLAGS)'"):
                for mode in ("direct", "ninja"):
                    with self.subTest(operator=operator, quoted=quoted, mode=mode):
                        (self.directory / "result").unlink(missing_ok=True)
                        assignment = quoted.format(op=operator)
                        self.makefile("EXTRA_FLAGS := -DREQUIRED\nall:\n"
                                      f"\t@$(MAKE) -f child.mk {assignment}\n")
                        if mode == "direct":
                            self.direct("all")
                        else:
                            self.direct("build", "-j2", "all")
                        self.assertEqual((self.directory / "result").read_text(),
                                         "-DREQUIRED")

    def test_deferred_assignments_after_recipe_prefixes(self):
        (self.directory / "child.mk").write_text(
            "all:\n\t@$(TOOL) '%s' done > result\n")
        for oneshell in (False, True):
            for preceding in ("", "\t@:\n"):
                for prefix in ("@", "+", "-", "@+-", "+ @ -", "  @+-"):
                    for mode in ("direct", "ninja"):
                        with self.subTest(oneshell=oneshell, preceding=preceding,
                                          prefix=prefix, mode=mode):
                            (self.directory / "result").unlink(missing_ok=True)
                            self.makefile((".ONESHELL:\n" if oneshell else "") +
                                          "RAW_TOOL := printf\nall:\n" + preceding +
                                          f"\t{prefix}TOOL='$$(RAW_TOOL)' $(MAKE) -f child.mk\n")
                            if mode == "direct":
                                self.direct("all")
                            else:
                                self.direct("build", "-j2", "all")
                            self.assertEqual((self.directory / "result").read_text(),
                                             "done")

    def test_inferred_depfile_preserves_compiler_failure(self):
        self.makefile("out.o: source.c\n"
                      "\t@clang -c -MMD -MF deps.d -o $@ $<\n")
        (self.directory / "source.c").write_text("#error expected failure\n")
        (self.directory / "deps.d").write_text("out.o: stale.h\n")
        self.direct("--ninja", "--detect_depfiles")
        status, output = self.run_command("sh", "./ninja.sh", "-j2", success=False)
        self.assertNotEqual(status, 0)
        self.assertIn("expected failure", output)
        self.assertFalse((self.directory / "deps.d.tmp").exists())

    def test_inferred_depfile_tracks_header_and_keeps_original(self):
        self.makefile("all: out.o\nout.o: source.c\n"
                      "\t@clang -c -MMD -MF deps.d -o $@ $<\n")
        (self.directory / "source.c").write_text(
            '#include "header.h"\nint value = VALUE;\n')
        header = self.directory / "header.h"
        header.write_text("#define VALUE 1\n")
        self.direct("--ninja")
        self.assertNotIn(" depfile =", (self.directory / "build.ninja").read_text())
        self.direct("--ninja", "--detect_depfiles")
        self.run_command("sh", "./ninja.sh", "-j2")
        original = (self.directory / "out.o").read_bytes()
        self.assertTrue((self.directory / "deps.d").exists())
        _, output = self.run_command("sh", "./ninja.sh", "-j2")
        self.assertIn("no work to do", output)
        header.write_text("#define VALUE 2\n")
        future = (self.directory / "out.o").stat().st_mtime + 2
        os.utime(header, (future, future))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertNotEqual((self.directory / "out.o").read_bytes(), original)

    def test_detection_declines_compound_recipes_without_changing_failure(self):
        self.makefile("all:\n\t@printf '%s' ' -c -MD -o out.o ' > result; false\n")
        self.direct("--ninja", "--detect_depfiles")
        self.assertNotIn(" depfile =", (self.directory / "build.ninja").read_text())
        status, _ = self.run_command("sh", "./ninja.sh", "-j2", success=False)
        self.assertNotEqual(status, 0)
        self.assertEqual((self.directory / "result").read_text(), " -c -MD -o out.o ")

    def test_translation_preserves_escaped_quotes_and_hashes(self):
        self.makefile(
            "all:\n"
            "\t@printf '%s' \"before\\\" # after\" > 'odd $$ name'\n"
            "\t@printf '%s' done >> 'odd $$ name'\n")
        self.direct("all")
        result = self.directory / "odd $ name"
        expected = result.read_bytes()
        self.assertEqual(expected, b'before" # afterdone')
        result.unlink()
        self.ninja()
        self.assertEqual(result.read_bytes(), expected)

    def test_explicit_depfile_overrides_detection_and_tracks_generated_header(self):
        self.makefile(
            "all: out.o\n"
            "out.o: .KATI_DEPFILE = explicit$$deps.d\n"
            "out.o: source.c | generated.h\n"
            "\t@clang -c -MMD -MF 'explicit$$deps.d' -o $@ $< && "
            "printf '%s' built >> log\n"
            "generated.h: seed\n\t@cp seed $@\n")
        (self.directory / "source.c").write_text(
            '#include "generated.h"\nint value = VALUE;\n')
        (self.directory / "seed").write_text("#define VALUE 1\n")
        self.direct("--ninja", "--detect_depfiles")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "log").read_text(), "built")
        header = self.directory / "generated.h"
        header.write_text("#define VALUE 2\n")
        future = (self.directory / "out.o").stat().st_mtime + 2
        os.utime(header, (future, future))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "log").read_text(), "builtbuilt")

    def test_translation_propagates_failure_between_recipe_lines(self):
        self.makefile("all:\n\t@printf '%s' 'literal # $$ ;' > result; false\n"
                      "\t@printf unexpected > should-not-exist\n")
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode):
                if mode == "direct":
                    status, _ = self.direct(success=False)
                else:
                    self.direct("--ninja")
                    status, _ = self.run_command("sh", "./ninja.sh", "-j2",
                                                 success=False)
                self.assertNotEqual(status, 0)
                self.assertFalse((self.directory / "should-not-exist").exists())
                self.assertEqual((self.directory / "result").read_text(),
                                 "literal # $ ;")

    def test_explain_resolves_included_rule_and_does_not_run_recipes(self):
        self.makefile("include rules.mk\n")
        (self.directory / "rules.mk").write_text(
            "out: VALUE = expanded\nout: input | ready\n"
            "\t+@printf '%s' '$(VALUE) $< $@' > $@\n"
            ".PHONY: ready\nready:\n\t+@touch ran\n")
        (self.directory / "input").write_text("source")
        for mode in ((), ("--ninja", "--regen")):
            _, output = self.direct(*mode, "--explain", "out")
            self.assertIn("Source: rules.mk:2", output)
            self.assertIn("Recipe source: rules.mk:3", output)
            self.assertIn("Prerequisites: input", output)
            self.assertIn("Order-only prerequisites: ready", output)
            self.assertIn("expanded input out", output)
            self.assertIn("outdated: output is missing", output)
            for name in ("out", "ran", "build.ninja", ".kati_stamp"):
                self.assertFalse((self.directory / name).exists(), name)

    def test_explain_freshness_and_order_only_inputs(self):
        self.makefile("out: input | ready\n\t@cp $< $@\n"
                      ".PHONY: ready\nready:\n\t@:\n")
        (self.directory / "input").write_text("source")
        (self.directory / "out").write_text("old")
        os.utime(self.directory / "input", (1000000000, 1000000000))
        os.utime(self.directory / "out", (1000000010, 1000000010))
        _, output = self.direct("--explain", "out")
        self.assertIn("Target: out\n  Source: Makefile:1\n"
                      "  Decision: up to date", output)
        self.assertIn("cp input out", output)
        self.assertNotIn("Outdated prerequisite: ready", output)
        os.utime(self.directory / "input", (1000000020, 1000000020))
        _, output = self.direct("--explain", "out")
        self.assertIn("outdated: prerequisite is newer", output)
        self.assertIn("Outdated prerequisite: input", output)
        self.assertEqual((self.directory / "out").read_text(), "old")

    def test_explain_propagates_planned_updates_and_labels_recursion(self):
        self.makefile("all: out\n\t+@$(MAKE) child\n"
                      "out: input\n\t@cp $< $@\n"
                      "input:\n\t@touch $@\n"
                      "child:\n\t@touch ran\n")
        (self.directory / "all").write_text("existing")
        (self.directory / "out").write_text("existing")
        os.utime(self.directory / "all", (1000000010, 1000000010))
        os.utime(self.directory / "out", (1000000000, 1000000000))
        _, output = self.direct("--explain", "all")
        self.assertIn("Outdated prerequisite: out (newer or would be rebuilt)", output)
        self.assertIn("child graph is not flattened", output)
        self.assertFalse((self.directory / "input").exists())
        self.assertFalse((self.directory / "ran").exists())

    def test_explain_regeneration_preserves_generated_files(self):
        self.makefile("out: input\n\t@cp $< $@\n")
        (self.directory / "input").write_text("source")
        self.direct("--ninja", "--regen", "out")
        names = ("build.ninja", "ninja.sh", "env.sh", ".kati_stamp")
        before = {name: (self.directory / name).read_bytes() for name in names}
        _, output = self.direct("--ninja", "--regen", "--explain", "out")
        self.assertIn("Regeneration: recorded inputs unchanged", output)
        self.makefile("out: input\n\t@printf changed > $@\n")
        _, output = self.direct("--ninja", "--regen", "--explain", "out")
        self.assertIn("Regeneration: required", output)
        self.assertIn("Makefile was modified", output)
        self.assertIn("printf changed > out", output)
        for name in names:
            self.assertEqual((self.directory / name).read_bytes(), before[name])

    def test_explain_double_colon_and_missing_input(self):
        self.makefile("out:: input\n\t@cp $< $@\nout::\n\t@touch $@\n")
        (self.directory / "input").write_text("source")
        (self.directory / "out").write_text("existing")
        _, output = self.direct("--explain", "out")
        self.assertIn("double-colon group is unconditional", output)
        self.assertIn("Double-colon group 2: outdated (unconditional)", output)
        self.assertIn("touch out", output)
        (self.directory / "input").unlink()
        status, output = self.direct("--explain", "out", success=False)
        self.assertNotEqual(status, 0)
        self.assertIn("missing input with no producing rule", output)

    def test_explain_forced_inputs_old_outputs_and_current_groups(self):
        self.makefile("out:: input\n\t@cp $< $@\n")
        (self.directory / "input").write_text("source")
        (self.directory / "out").write_text("existing")
        os.utime(self.directory / "input", (1000000000, 1000000000))
        os.utime(self.directory / "out", (1000000010, 1000000010))
        for mode in ((), ("--ninja", "--regen")):
            _, output = self.direct(*mode, "--explain", "out")
            self.assertIn("all double-colon groups are current", output)
            self.assertIn("cp input out", output)
            _, output = self.direct(*mode, "--explain", "-W", "input", "out")
            self.assertIn("input (forced by -W)", output)
            _, output = self.direct(*mode, "--explain", "-W", "input",
                                     "-o", "out", "out")
            self.assertIn("kept unchanged by -o", output)

    def test_explain_vpath_and_target_specific_exports(self):
        (self.directory / "src").mkdir()
        (self.directory / "src" / "input").write_text("source")
        self.makefile("VPATH = src\nexport VALUE = top\nout: VALUE = local\n"
                      "out: input\n\t@printf '%s' '$(VALUE) $<' > $@\n")
        for mode in ((), ("--ninja", "--regen")):
            _, output = self.direct(*mode, "--explain", "out")
            self.assertIn("Prerequisites: src/input", output)
            self.assertIn("local src/input", output)
            self.assertFalse((self.directory / "out").exists())

    def test_explain_option_value_is_part_of_regeneration_identity(self):
        # A Makefile can itself be named like the diagnostic option.
        (self.directory / "--explain").write_text("all:\n\t@:\n")
        args = ("--ninja", "--regen", "-f", "--explain", "all")
        self.direct(*args)
        _, output = self.direct(*args, "--explain", "--explain")
        self.assertIn("Regeneration: recorded inputs unchanged", output)

    def test_explain_requires_current_generated_makefiles(self):
        self.makefile("include generated.mk\nall:\n\t@touch ran\n"
                      "generated.mk:\n\t@printf 'X = value\\n' > $@\n")
        status, output = self.direct("--explain", "all", success=False)
        self.assertNotEqual(status, 0)
        self.assertIn("generated Makefiles need rebuilding", output)
        self.assertFalse((self.directory / "generated.mk").exists())
        self.assertFalse((self.directory / "ran").exists())

    def test_strict_compat_rejects_known_gaps_with_source(self):
        self.makefile(".NOTINTERMEDIATE: out\nall:\n\t@:\n")
        for mode in ((), ("--ninja",)):
            status, output = self.direct(*mode, "--strict-compat", success=False)
            self.assertNotEqual(status, 0)
            self.assertIn("Makefile:1:", output)
            self.assertIn("--strict-compat: .NOTINTERMEDIATE", output)
        self.makefile("all: temp\n.INTERMEDIATE: temp\ntemp:\n\t@touch $@\n")
        self.direct("--strict-compat", "all")
        self.assertFalse((self.directory / "temp").exists())
        for extra in ((), ("--explain",)):
            status, output = self.direct("--ninja", "--strict-compat", *extra,
                                         "all", success=False)
            self.assertNotEqual(status, 0)
            self.assertIn("intermediate target 'temp'", output)
        self.assertFalse((self.directory / "build.ninja").exists())

    def test_strict_compat_rejects_recipe_eval_and_preserves_graph(self):
        self.makefile("all:\n\t@:\n")
        self.direct("--ninja", "--regen")
        before = (self.directory / ".kati_stamp").read_bytes()
        self.makefile("all:\n\t$(eval X := changed)\n\t@printf '$(X)' > result\n")
        status, output = self.direct("--ninja", "--strict-compat", success=False)
        self.assertNotEqual(status, 0)
        self.assertIn("Makefile:2:", output)
        self.assertIn("$(eval) in a recipe", output)
        self.assertEqual((self.directory / ".kati_stamp").read_bytes(), before)
        self.direct("--strict-compat")
        self.assertEqual((self.directory / "result").read_text(), "changed")

    def test_strict_compat_accepts_recursive_builds_and_parse_time_eval(self):
        self.makefile("$(eval VALUE := preserved)\nall:\n\t@$(MAKE) child\n"
                      "child:\n\t@printf '$(VALUE)' > result\n")
        self.direct("--strict-compat", "-j2")
        self.assertEqual((self.directory / "result").read_text(), "preserved")
        (self.directory / "result").unlink()
        self.direct("--ninja", "--regen", "--strict-compat")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text(), "preserved")

    def test_strict_compat_propagates_to_recursive_children(self):
        self.makefile("all:\n\t@$(MAKE) -f child.mk child\n")
        (self.directory / "child.mk").write_text(
            ".NOTINTERMEDIATE: child\nchild:\n\t@touch ran\n")
        for mode in ("direct", "ninja"):
            if mode == "direct":
                status, output = self.direct("--strict-compat", success=False)
            else:
                self.direct("--ninja", "--strict-compat")
                status, output = self.run_command("sh", "./ninja.sh", "-j2",
                                                  success=False)
            self.assertNotEqual(status, 0)
            self.assertIn("child.mk:1:", output)
            self.assertIn("--strict-compat: .NOTINTERMEDIATE", output)
            self.assertFalse((self.directory / "ran").exists())

    def test_explain_rejects_conflicting_execution_modes(self):
        self.makefile("all:\n\t+@touch ran\n")
        for mode in ("-t", "-n", "-q"):
            status, output = self.direct("--explain", mode, success=False)
            self.assertNotEqual(status, 0)
            self.assertIn("--explain cannot be combined", output)
        status, output = self.direct("build", "--explain", success=False)
        self.assertNotEqual(status, 0)
        self.assertIn("use omktr --ninja", output)
        self.assertFalse((self.directory / "ran").exists())

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kati-correctness-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def makefile(self, text):
        (self.directory / "Makefile").write_text(text, encoding="utf-8")

    def run_command(self, *args, success=True):
        profile = os.environ.get("OMKTR_TEST_GRAPH_PROFILE")
        if (profile and args and args[0] == str(KATI) and
                ("--ninja" in args or (len(args) > 1 and args[1] == "build")) and
                not any(arg.startswith("--graph_optimizations=") for arg in args)):
            if profile not in ("off", "safe", "aggressive"):
                raise ValueError("Invalid OMKTR_TEST_GRAPH_PROFILE")
            insertion = args.index("--") if "--" in args else len(args)
            args = (*args[:insertion], f"--graph_optimizations={profile}", *args[insertion:])
        result = subprocess.run(
            args, cwd=self.directory, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=15, check=False,
        )
        output = result.stdout.decode("utf-8", "replace")
        if success:
            self.assertEqual(result.returncode, 0, output)
        return result.returncode, output

    def direct(self, *args, success=True):
        return self.run_command(str(KATI), *args, success=success)

    def ninja(self):
        self.direct("--ninja", "--regen")
        return self.run_command("sh", "./ninja.sh", "-j2")

    def test_build_refreshes_preserved_timestamps_and_generated_includes(self):
        self.makefile("include config.mk\n.PHONY: all\nall:\n"
                      "\t@printf '%s' '$(VALUE)' > result\n"
                      "config.mk: seed\n\t@cp seed $@\n")
        (self.directory / "seed").write_text("VALUE := one\n")
        (self.directory / "config.mk").write_text("VALUE := one\n")
        self.direct("build")
        (self.directory / "seed").write_text("VALUE := two\n")
        self.direct("build")
        self.assertEqual((self.directory / "result").read_text(), "two")
        self.makefile(".PHONY: all\nall:\n\t@printf one > result\n")
        self.direct("build")
        stamp = (self.directory / "Makefile").stat()
        self.makefile(".PHONY: all\nall:\n\t@printf two > result\n")
        os.utime(self.directory / "Makefile", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.direct("build")
        self.assertEqual((self.directory / "result").read_text(), "two")

    def test_build_repeats_observable_evaluation_output(self):
        self.makefile("$(info evaluated)\n$(warning diagnostic)\nall:\n\t@:\n")
        for _ in range(2):
            _, output = self.direct("build")
            self.assertIn("evaluated", output)
            self.assertIn("diagnostic", output)

    def test_recursive_cache_hits_and_invalidates_without_caching_work(self):
        child = self.directory / "child"
        child.mkdir()
        source = "include empty.mk\nall: out\nout: input\n\t@cp $< $@\n"
        (child / "Makefile").write_text(source)
        (child / "empty.mk").write_text("")
        (child / "input").write_text("one")
        self.makefile(".PHONY: all\nall:\n"
                      "\t@$(MAKE) --recursive_cache --kati_stats -C child all\n")
        self.direct("-j32")
        cache = child / ".openmktr-recursive-cache"
        self.assertTrue(cache.exists())
        _, output = self.direct("-j32")
        self.assertIn("Recursive cache hit", output)
        self.assertNotIn("eval time:", output)
        (child / "out").unlink()
        _, output = self.direct("-j32")
        self.assertNotIn("Recursive cache hit", output)
        self.assertIn("Recursive cache eligibility reused", output)
        self.assertEqual((child / "out").read_text(), "one")
        cache.write_bytes(b"partial certificate")
        _, output = self.direct("-j32")
        self.assertNotIn("Recursive cache hit", output)
        self.assertNotIn("Recursive cache eligibility reused", output)
        stamp = (child / "Makefile").stat()
        (child / "Makefile").write_text(source.replace("cp $< $@", "printf two > $@"))
        os.utime(child / "Makefile", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        (child / "out").unlink()
        _, output = self.direct("-j32")
        self.assertNotIn("Recursive cache hit", output)
        self.assertNotIn("Recursive cache eligibility reused", output)
        self.assertEqual((child / "out").read_text(), "two")

    def test_recursive_cache_declines_dynamic_evaluation(self):
        child = self.directory / "child"
        child.mkdir()
        self.makefile("all:\n\t@$(MAKE) --recursive_cache --kati_stats -C child all\n")
        for source in ("$(info observable)\nall:\n\t@:\n",
                       "X := $(shell printf dynamic)\nall:\n\t@:\n",
                       "export X := exported\nall:\n\t@:\n",
                       ".PHONY: all\nall:\n\t@printf sideeffect > result\n"):
            (child / "Makefile").write_text(source)
            for _ in range(2):
                _, output = self.direct("-j32")
                self.assertNotIn("Recursive cache hit", output)
                self.assertFalse((child / ".openmktr-recursive-cache").exists())

    def test_recursive_cache_declines_untracked_search_candidates(self):
        child = self.directory / "child"
        child.mkdir()
        (child / "headers").mkdir()
        (child / "headers/config.mk").write_text("CONFIG := first\n")
        (child / "headers/input").write_text("input")
        self.makefile("all:\n\t@$(MAKE) --recursive_cache --kati_stats -C child "
                      "-I headers $(OPTIONS) all\n")
        for source, option in (
                ("include config.mk\nall: out\nout:\n\t@printf '%s' '$(CONFIG)' > $@\n", ""),
                ("all: out\nout: input\n\t@cp $< $@\n", "VPATH=headers")):
            with self.subTest(option=option):
                (child / "Makefile").write_text(source)
                (child / "out").unlink(missing_ok=True)
                for _ in range(2):
                    _, output = self.direct("OPTIONS=" + option)
                    self.assertNotIn("Recursive cache hit", output)
                    self.assertFalse((child / ".openmktr-recursive-cache").exists())
                if not option:
                    (child / "config.mk").write_text("CONFIG := second\nout: newer\n")
                    (child / "newer").write_text("new prerequisite")
                    self.direct("OPTIONS=")
                    self.assertEqual((child / "out").read_text(), "second")

    def test_nested_ninja_respects_fifo_and_single_reservation(self):
        children = [f"child{i}" for i in range(4)]
        self.makefile(".PHONY: all " + " ".join(children) + "\nall: " +
                      " ".join(children) + "\n" + "".join(
                          f"{name}:\n\t@$(MAKE) build -C {name} all\n"
                          for name in children))
        for name in children:
            (self.directory / name).mkdir()
            (self.directory / name / "Makefile").write_text(
                ".PHONY: all a b c d\nall: a b c d\na b c d:\n"
                "\t@python3 ../worker.py\n")
        (self.directory / "worker.py").write_text(
            "import fcntl, json, time\n"
            "def update(delta):\n"
            "    with open('../counts', 'r+') as f:\n"
            "        fcntl.flock(f, fcntl.LOCK_EX)\n"
            "        active, peak, done = json.load(f)\n"
            "        active += delta\n"
            "        f.seek(0)\n"
            "        json.dump([active, max(active, peak), done + (delta < 0)], f)\n"
            "        f.truncate()\n"
            "update(1)\ntime.sleep(0.02)\nupdate(-1)\n")
        self.direct("--ninja", "-j32")
        fifo = self.directory / "fifo"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        for jobs, reserved in ((1, False), (4, False), (1, True), (4, True)):
            with self.subTest(jobs=jobs, reserved=reserved):
                tokens = b"x" * (jobs - int(reserved))
                os.write(fd, tokens)
                env = dict(os.environ, KATI_JOBSERVER_FIFO=str(fifo), KATI_JOBS=str(jobs))
                env["KATI_JOBSERVER_RESERVED"] = "1" if reserved else "0"
                (self.directory / "counts").write_text("[0, 0, 0]")
                result = subprocess.run(["sh", "./ninja.sh", "-j32"],
                                        cwd=self.directory, env=env,
                                        capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                active, peak, done = json.loads((self.directory / "counts").read_text())
                self.assertEqual((active, done), (0, 16))
                self.assertLessEqual(peak, jobs)
                if tokens:
                    self.assertEqual(os.read(fd, 100), tokens)
                else:
                    with self.assertRaises(BlockingIOError):
                        os.read(fd, 100)

    def test_ninja_jobserver_helper_preserves_output_and_large_recipes(self):
        self.makefile("all:\n\t@printf '%s' '" + "x" * 150000 + "' > result\n"
                      "\t@printf 'stdout\\n'; printf 'stderr\\n' >&2\n")
        _, output = self.ninja()
        self.assertIn("stdout", output)
        self.assertIn("stderr", output)
        self.assertEqual((self.directory / "result").read_text(), "x" * 150000)

    def test_multi_option_shell_flags_in_direct_ninja_and_response_files(self):
        for flags in ("-e -c", "-e '-c'"):
            for large in (False, True):
                for mode in ("direct", "ninja"):
                    with self.subTest(flags=flags, large=large, mode=mode):
                        result = self.directory / "result"
                        result.unlink(missing_ok=True)
                        value = "x" * (150000 if large else 1)
                        self.makefile(".SHELLFLAGS := " + flags + "\nall:\n"
                                      "\t@printf '%s' '" + value + "' > result\n")
                        if mode == "direct":
                            self.direct("-j32")
                        else:
                            self.ninja()
                        self.assertEqual(result.read_text(), value)
                        result.unlink()
                        self.makefile(".SHELLFLAGS := " + flags + "\nall:\n"
                                      "\t@false; printf '%s' '" + value + "' > result\n")
                        if mode == "direct":
                            status, _ = self.direct("-j32", success=False)
                        else:
                            self.direct("--ninja", "--regen")
                            status, _ = self.run_command("sh", "./ninja.sh", "-j2", success=False)
                        self.assertNotEqual(status, 0)
                        self.assertFalse(result.exists())

    def test_recursive_cache_contention_does_not_deadlock_same_directory(self):
        self.makefile("all:\n\t@$(MAKE) --recursive_cache child\n"
                      "child:\n\t@$(MAKE) --recursive_cache leaf\n"
                      "leaf:\n\t@printf ready > result\n")
        self.direct("-j32")
        self.assertEqual((self.directory / "result").read_text(), "ready")

    def test_recursive_cache_configuration_invalidates_with_outputs_present(self):
        child = self.directory / "child"
        child.mkdir()
        (child / "input").write_text("one")
        (child / "config.mk").write_text("CONFIG := one\n")
        (child / "Makefile").write_text(
            "include config.mk\nall: out\nout: input\n\t@cp $< $@\n")
        self.makefile(".PHONY: all\nall:\n"
                      "\t@$(MAKE) --recursive_cache --kati_stats -C child MODE=$(MODE)\n")
        self.direct("MODE=one")
        _, output = self.direct("MODE=one")
        self.assertIn("Recursive cache hit", output)
        _, output = self.direct("MODE=two")
        self.assertNotIn("Recursive cache hit", output)
        stamp = (child / "config.mk").stat()
        (child / "config.mk").write_text("CONFIG := two\n")
        os.utime(child / "config.mk", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        _, output = self.direct("MODE=two")
        self.assertNotIn("Recursive cache hit", output)
        for value, hit in (("one", False), ("one", True), ("two", False), ("two", True)):
            result = subprocess.run(
                [str(KATI), "MODE=two"], cwd=self.directory, capture_output=True,
                env=dict(os.environ, OMKTR_CACHE_KEY_TEST=value), timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(b"Recursive cache hit" in result.stderr, hit)
        # Configuration-only invalidation must not invent Make rebuilds.
        self.assertEqual((child / "out").read_text(), "one")
        (child / "input").write_text("two")
        _, output = self.direct("MODE=two")
        self.assertNotIn("Recursive cache hit", output)
        self.assertEqual((child / "out").read_text(), "two")

    def test_build_refreshes_inputs_and_preserves_project_graph(self):
        project_graph = self.directory / "build.ninja"
        project_graph.write_text("# unrelated project graph\n")
        include = self.directory / "config.mk"
        include.write_text("VALUE = first\n")
        self.makefile("include config.mk\nall:\n"
                      "\t@printf '%s' '$(VALUE):$(MODE)' > result\n")
        self.direct("build", "-j2", "all", "MODE=one")
        self.assertEqual((self.directory / "result").read_text(), "first:one")
        graph = self.directory / ".openmktr/build.ninja"
        graph_time = graph.stat().st_mtime_ns
        _, output = self.direct("build", "-j2", "all", "MODE=one")
        self.assertIn("No need to regenerate", output)
        self.assertEqual(graph.stat().st_mtime_ns, graph_time)
        include.write_text("VALUE = second\n")
        self.direct("build", "-j2", "all", "MODE=two")
        self.assertEqual((self.directory / "result").read_text(), "second:two")
        self.makefile("all:\n\t@printf changed > result\n")
        self.direct("build", "-j2", "all")
        self.assertEqual((self.directory / "result").read_text(), "changed")
        self.assertEqual(project_graph.read_text(), "# unrelated project graph\n")

    def test_build_tracks_environment_and_missing_support_files(self):
        self.makefile("all:\n\t@printf '%s' '$(BUILD_VALUE)' > result\n")
        for value in ("first", "second"):
            result = subprocess.run(
                [str(KATI), "build", "all"], cwd=self.directory,
                env=dict(os.environ, BUILD_VALUE=value), capture_output=True,
                timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((self.directory / "result").read_text(), value)
        (self.directory / ".openmktr/env.sh").unlink()
        self.direct("build", "all")
        self.assertTrue((self.directory / ".openmktr/env.sh").exists())

    def test_build_stops_on_generation_or_recipe_failure(self):
        self.makefile("all:\n\t@printf old >> ran\n")
        self.direct("build", "all")
        self.makefile("$(error generation failed)\nall:\n\t@printf stale >> ran\n")
        status, _ = self.direct("build", "all", success=False)
        self.assertNotEqual(status, 0)
        self.assertEqual((self.directory / "ran").read_text(), "old")
        self.makefile("all:\n\t@false\n")
        status, _ = self.direct("build", "all", success=False)
        self.assertNotEqual(status, 0)

    def test_build_serializes_generation_and_execution(self):
        self.makefile(".PHONY: all\nall:\n"
                      "\t@mkdir running; sleep 0.2; "
                      "printf '%s\\n' '$(VALUE)' >> results; rmdir running\n")
        builds = [subprocess.Popen(
            [str(KATI), "build", "-j2", "all", "VALUE=" + value],
            cwd=self.directory, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for value in ("first", "second")]
        for process in builds:
            output, _ = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 0, output)
        self.assertCountEqual((self.directory / "results").read_text().splitlines(),
                              ["first", "second"])

    def test_build_directory_paths_and_recursive_exports(self):
        child = self.directory / "project"
        child.mkdir()
        (child / "Makefile").write_text(
            "export VALUE = $(MODE)\nall:\n\t@$(MAKE) -f child.mk\n")
        (child / "child.mk").write_text(
            "all:\n\t@printf '%s' '$(VALUE)' > result\n")
        for output_dir in ("graph ' dir", str(child / "absolute graph")):
            self.direct("build", "-C", "project", "--ninja_dir", output_dir,
                        "-j2", "all", "MODE=exported")
            self.assertEqual((child / "result").read_text(), "exported")

    def test_build_refuses_unrelated_explicit_output(self):
        self.makefile("all:\n\t@touch result\n")
        graph = self.directory / "build.ninja"
        graph.write_text("# project-owned\n")
        status, output = self.direct("build", "--ninja_dir=.", success=False)
        self.assertNotEqual(status, 0)
        self.assertIn("refusing to overwrite", output)
        self.assertEqual(graph.read_text(), "# project-owned\n")
        self.assertFalse((self.directory / "result").exists())

    def test_build_argument_boundaries_invalidate_graph(self):
        self.makefile(".PHONY: all y\nall y:\n"
                      "\t@printf '%s' '$(A):$(B)' > result\n")
        self.direct("build", "all", "A=x y", "B=z")
        self.assertEqual((self.directory / "result").read_text(), "x y:z")
        self.direct("build", "all", "A=x", "y", "B=z")
        self.assertEqual((self.directory / "result").read_text(), "x:z")

    def test_build_restarts_for_generated_includes(self):
        self.makefile("include generated.mk\nall:\n"
                      "\t@printf '%s' '$(VALUE)' > result\n"
                      "generated.mk:\n\t@printf 'VALUE = remade\\n' > $@\n")
        self.direct("build", "all")
        self.assertEqual((self.directory / "result").read_text(), "remade")

    def test_build_stdin_and_dry_run(self):
        for value in ("first", "second"):
            result = subprocess.run(
                [str(KATI), "build", "-f", "-", "all"], cwd=self.directory,
                input="all:\n\t@printf " + value + " > result\n", text=True,
                capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((self.directory / "result").read_text(), value)
        self.makefile("all:\n\t@touch dry-run-result\n")
        self.direct("build", "-n", "all")
        self.assertFalse((self.directory / "dry-run-result").exists())

    def test_build_interrupt_releases_lock(self):
        self.makefile("all:\n\t@touch started; sleep 10; touch stale\n")
        process = subprocess.Popen(
            [str(KATI), "build", "all"], cwd=self.directory,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not (self.directory / "started").exists():
                self.assertIsNone(process.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            process.terminate()
            process.communicate(timeout=5)
            self.assertNotEqual(process.returncode, 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
        self.makefile("all:\n\t@touch recovered\n")
        self.direct("build", "all")
        self.assertTrue((self.directory / "recovered").exists())
        self.assertFalse((self.directory / "stale").exists())

    def test_wait_barriers_distinguish_slashes_from_underscores(self):
        makefile = (
            ".PHONY: all a/b a_b first second third fourth\n"
            "all: a/b a_b\n"
            "a/b: first .WAIT second\n"
            "a_b: third .WAIT fourth\n"
            "first:\n\t@touch first.done\n"
            "second:\n\t@test -f first.done\n"
            "third:\n\t@sleep 0.1; touch third.done\n"
            "fourth:\n\t@test -f third.done\n")
        for target in ("a/b", "a/" + "b" * 60 + "/" + "c" * 60):
            with self.subTest(target=target):
                self.makefile(makefile.replace("a/b", target).replace(
                    "a_b", target.replace("/", "_")))
                self.direct("--ninja", "all")
                graph = (self.directory / "build.ninja").read_text()
                barriers = [line.split(":", 1)[0] for line in graph.splitlines()
                            if line.startswith("build .kati_wait_")]
                self.assertEqual(len(barriers), 2, graph)
                self.assertEqual(len(set(barriers)), 2, graph)
                self.run_command("sh", "./ninja.sh", "-j4")
                (self.directory / "first.done").unlink()
                (self.directory / "third.done").unlink()
                self.direct("-j4", "all")
                (self.directory / "first.done").unlink()
                (self.directory / "third.done").unlink()

    def test_version_is_compatible_with_make_host_checks(self):
        _, output = self.direct("--version")
        self.assertRegex(output, r"\AGNU Make 4\.2\.1\nomktr [^\n]+\n\Z")
        _, short_output = self.direct("-v")
        self.assertEqual(short_output, output)

        # Buildroot's check-host-make.sh extracts the first version line
        # with these sed expressions before comparing major and minor.
        version = subprocess.run(
            ["sed", "-e", r"s/^.* \([0-9\.]\)/\1/g",
             "-e", r"s/[-\n].*//g", "-e", "1q"],
            input=output, text=True, capture_output=True, check=True,
        ).stdout.strip()
        self.assertEqual(version, "4.2.1")

    def test_ninja_reuses_default_shell(self):
        self.makefile("all:\n"
                      "\t@value='space and quote'\"'\"; export value; "
                      "sh -c 'printf \"%s\" \"$$value\"' > result\n")
        self.direct("all")
        expected = (self.directory / "result").read_text()
        self.assertEqual(expected, "space and quote'")
        (self.directory / "result").unlink()
        self.direct("--ninja", "all")
        graph = (self.directory / "build.ninja").read_text()
        commands = [line for line in graph.splitlines()
                    if line.startswith(" command = ")]
        self.assertEqual(len(commands), 1, graph)
        expanded = self.run_command("ninja", "-t", "commands")[1]
        self.assertTrue(expanded.lstrip().startswith("cd "), expanded)
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text(), expected)

    def test_ninja_preserves_custom_shell_flags(self):
        self.makefile(".SHELLFLAGS := -ec\n"
                      "all:\n\t@false; printf unexpected > result\n")
        status, _ = self.direct("all", success=False)
        self.assertNotEqual(status, 0)
        self.assertFalse((self.directory / "result").exists())
        self.direct("--ninja", "all")
        status, _ = self.run_command("sh", "./ninja.sh", success=False)
        self.assertNotEqual(status, 0)
        self.assertFalse((self.directory / "result").exists())

    def test_ninja_preserves_custom_shell(self):
        shell = self.directory / "recipe-shell"
        shell.write_text("#!/bin/sh\nprintf x >> shell-used\n"
                         'exec /bin/sh "$@"\n', encoding="utf-8")
        shell.chmod(0o755)
        self.makefile(f"SHELL := {shell}\n"
                      "all:\n\t@printf done > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "shell-used").read_text(), "x")
        (self.directory / "result").unlink()
        self.direct("--ninja", "all")
        self.run_command("sh", "./ninja.sh")
        self.assertEqual((self.directory / "shell-used").read_text(), "xx")
        self.assertEqual((self.directory / "result").read_text(), "done")

    def test_undefine_feature_matches_supported_directive(self):
        self.makefile("ifeq ($(filter undefine,$(.FEATURES)),)\n"
                      "$(error undefine feature missing)\nendif\n"
                      "VALUE := stale\nundefine VALUE\n"
                      "all:\n\t@printf '%s' '$(origin VALUE)' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "undefined")

    def test_cleared_makeoverrides_stops_recursive_command_line_override(self):
        self.makefile("MAKEOVERRIDES =\nall:\n\t@$(MAKE) -f child.mk\n")
        (self.directory / "child.mk").write_text(
            "VALUE = child\nall:\n\t@printf '%s' '$(VALUE)' > result\n")
        self.direct("VALUE=parent", "all")
        self.assertEqual((self.directory / "result").read_text(), "child")

    def test_builtin_object_link_rule_honors_explicit_objects(self):
        (self.directory / "program.c").write_text(
            "int helper(void); int main(void) { return helper() != 7; }\n")
        (self.directory / "helper.c").write_text(
            "int helper(void) { return 7; }\n")
        self.makefile("program: program.o helper.o\n")
        self.direct("program")
        self.run_command("./program")

    def test_builtin_c_source_links_program(self):
        (self.directory / "program.c").write_text(
            "int main(void) { return 0; }\n")
        self.makefile("program: program.c\n")
        self.direct("program")
        self.run_command("./program")

    def test_builtin_c_source_links_undeclared_program(self):
        (self.directory / "program.c").write_text(
            "int main(void) { return 0; }\n")
        self.makefile("all: program\n")
        self.direct("all")
        self.run_command("./program")

    def test_directory_pattern_beats_basename_link_pattern(self):
        (self.directory / "bin-wrappers").mkdir()
        (self.directory / "wrap-for-bin.sh").write_text("wrapper\n")
        (self.directory / "bin-wrappers/receive-pack.o").write_text(
            "not an object\n")
        self.makefile("git-%: %.o\n"
                      "\t@cp $< $@\n"
                      "bin-wrappers/%: wrap-for-bin.sh\n"
                      "\t@cp $< $@\n")
        self.direct("bin-wrappers/git-receive-pack")
        self.assertEqual((self.directory / "bin-wrappers/git-receive-pack")
                         .read_text(), "wrapper\n")

    def test_builtin_cpp_variable_preprocesses_generated_input(self):
        self.makefile("all:\n\t@$(CPP) -P - < input.c > output\n")
        (self.directory / "input.c").write_text("#define VALUE 37\nVALUE\n")
        self.direct("all")
        self.assertEqual((self.directory / "output").read_text().strip(), "37")

    def test_ninja_question_uses_nanosecond_prerequisite_times(self):
        self.makefile(
            "all: output\n"
            "output: first second\n"
            "\t@printf '%s' \"$?\" > output\n"
        )
        first = self.directory / "first"
        second = self.directory / "second"
        output = self.directory / "output"
        first.write_text("first\n")
        second.write_text("second\n")
        self.direct("--ninja", "all")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual(output.read_text(), "first second kati_missing_output")

        # Put the output later in the same second as both inputs, then move
        # one prerequisite 250 ms ahead without relying on sleeps or crossing
        # a wall-clock second boundary.
        os.utime(first, ns=(1_000_000_000, 1_000_000_000))
        os.utime(second, ns=(1_500_000_000, 1_500_000_000))
        os.utime(output, ns=(2_000_000_000, 2_000_000_000))
        os.utime(second, ns=(2_250_000_000, 2_250_000_000))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual(output.read_text(), "second kati_missing_output")

    def test_ninja_question_preserves_low_resolution_prerequisite_semantics(self):
        self.makefile(
            ".LOW_RESOLUTION_TIME: second\n"
            "all: output\n"
            "output: first second\n"
            "\t@printf '%s' \"$?\" > output\n"
        )
        first = self.directory / "first"
        second = self.directory / "second"
        output = self.directory / "output"
        first.write_text("first\n")
        second.write_text("second\n")
        self.direct("--ninja", "all")
        self.run_command("sh", "./ninja.sh", "-j2")

        base = (int(time.time()) - 10) * 1_000_000_000
        os.utime(first, ns=(base, base))
        os.utime(second, ns=(base + 750_000_000, base + 750_000_000))
        os.utime(output, ns=(base + 500_000_000, base + 500_000_000))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual(output.read_text(), "first second kati_missing_output")

        os.utime(second, ns=(base + 2_000_000_000, base + 2_000_000_000))
        os.utime(output, ns=(base + 1_500_000_000, base + 1_500_000_000))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual(output.read_text(), "second kati_missing_output")

    def test_multiline_function_call_in_define(self):
        self.makefile(
            "define PROVIDE\n"
            "$(strip\n"
            "  $(if $(filter @%,$(1)),\n"
            "    $(patsubst @%,%,$(1)),\n"
            "    missing\n"
            "  )\n"
            ")\n"
            "endef\n"
            "VALUE := $(call PROVIDE,@wget-any)\n"
            "all:\n\t@printf '%s' '$(VALUE)' > result\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "wget-any")

    def test_environment_override_flag_and_recursion(self):
        self.makefile(
            "VALUE = file\n"
            "VALUE += appended\n"
            "override FORCED = override\n"
            "all:\n"
            "\t@printf '%s|%s|%s|%s' '$(VALUE)' '$(origin VALUE)' "
            "'$(FORCED)' '$(origin FORCED)' > parent-result\n"
            "\t@$(MAKE) -s -f child.mk\n"
        )
        (self.directory / "child.mk").write_text(
            "VALUE = child\n"
            "all:\n\t@printf '%s|%s' '$(VALUE)' '$(origin VALUE)' "
            "> result\n"
        )
        env = os.environ.copy()
        env.update(VALUE="environment", FORCED="environment", MAKEFLAGS="",
                   SHELL="/bin/false")
        completed = subprocess.run(
            [str(KATI), "-se", "all"], cwd=self.directory, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15,
            check=False,
        )
        self.assertEqual(completed.returncode, 0,
                         completed.stdout.decode("utf-8", "replace"))
        self.assertEqual((self.directory / "parent-result").read_text(),
                         "environment|environment override|override|override")
        self.assertEqual((self.directory / "result").read_text(),
                         "environment|environment override")

    def test_command_line_beats_environment_override(self):
        self.makefile("VALUE = file\nall:\n\t@printf '%s|%s' "
                      "'$(VALUE)' '$(origin VALUE)' > result\n")
        env = os.environ.copy()
        env["VALUE"] = "environment"
        completed = subprocess.run(
            [str(KATI), "-e", "VALUE=command", "all"], cwd=self.directory,
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=15, check=False,
        )
        self.assertEqual(completed.returncode, 0,
                         completed.stdout.decode("utf-8", "replace"))
        self.assertEqual((self.directory / "result").read_text(),
                         "command|command line")

    def test_implicit_prerequisite_wildcard_expands_after_stem(self):
        (self.directory / "src/common").mkdir(parents=True)
        (self.directory / "out").mkdir()
        (self.directory / "src/common/debug.c").write_text("source\n")
        self.makefile(
            "out/%.o: src/*/%.c\n"
            "\t@printf '%s\\n' '$<' > $@\n"
        )
        self.direct("out/debug.o")
        self.assertEqual((self.directory / "out/debug.o").read_text(),
                         "src/common/debug.c\n")

    def test_single_suffix_rule_uses_vpath_for_dotless_target(self):
        source = self.directory / "source/locale"
        source.mkdir(parents=True)
        (source / "XLC_LOCALE.pre").write_text("locale data\n")
        self.makefile(
            "VPATH = source\n"
            ".SUFFIXES:\n"
            ".SUFFIXES: .pre\n"
            ".pre:\n"
            "\t@mkdir -p $(@D); cat $< > $@\n"
        )
        self.direct("locale/XLC_LOCALE")
        self.assertEqual((self.directory / "locale/XLC_LOCALE").read_text(),
                         "locale data\n")

    def test_tab_indented_conditional_after_inactive_diagnostic(self):
        self.makefile(
            "ifeq (1,1)\n"
            "\tifeq (0,1)\n"
            "\t$(error inactive diagnostic)\n"
            "\tendif\n"
            "else\n"
            "\t$(error inactive branch)\n"
            "endif\n"
            "all:\n"
            "\t@printf ready > result\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "ready")

    def test_abbreviated_no_print_directory_option(self):
        self.makefile("all:\n\t@printf pass > result\n")
        self.direct("--no-print-dir", "all")
        self.assertEqual((self.directory / "result").read_text(), "pass")

    def test_dash_makefile_reads_standard_input(self):
        result = subprocess.run(
            [str(KATI), "-f", "-", "am--depfiles"],
            cwd=self.directory,
            input=b".PHONY: am--depfiles\nam--depfiles:\n\t@printf ready > result\n",
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=15, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual((self.directory / "result").read_text(), "ready")

    def test_recipe_stderr_keeps_its_stream_without_output_sync(self):
        self.makefile("all:\n\t@printf 'out\\n'\n\t@printf 'err\\n' >&2\n")
        result = subprocess.run(
            [str(KATI), "all"], cwd=self.directory,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=15, check=False,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"out\n")
        self.assertEqual(result.stderr, b"err\n")

    def test_command_line_variables_reach_plain_recipe_environment(self):
        self.makefile(
            "unexport HIDDEN\n"
            "all:\n\t@printf '%s/%s/%s' \"$$MAKE\" \"$$FOO\" "
            "\"$${HIDDEN-unset}\" > result\n"
        )
        self.direct("MAKE=/bin/echo", "FOO=bar", "HIDDEN=secret", "all")
        self.assertEqual((self.directory / "result").read_text(),
                         "/bin/echo/bar/unset")

    def test_empty_shell_output_in_recipe(self):
        self.makefile("all:\n\t@printf '%s' 'a$(shell :)b' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "ab")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "ab")

    def test_realpath_handles_multiple_words(self):
        (self.directory / "first").touch()
        (self.directory / "second").touch()
        self.makefile("PATHS := $(realpath first missing second)\n"
                      "all:\n\t@printf '%s' '$(PATHS)' > result\n")
        expected = f"{self.directory}/first {self.directory}/second"
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), expected)
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), expected)

    def test_whitespace_only_lines(self):
        self.makefile(" \n\t\nall:\n\t\n\t@printf ok > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "ok")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "ok")

    def test_expanded_prerequisite_with_equals_is_a_filename(self):
        prerequisite = self.directory / "icon=1.png"
        prerequisite.write_text("image")
        self.makefile("FILES := $(shell printf 'icon=1.png')\n"
                      "all: $(FILES)\n\t@cat $^ > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "image")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "image")

    def test_expanded_rule_assignment_survives_evaluation(self):
        self.makefile("RULE := all: VALUE=kept\n$(RULE)\n"
                      "all: $(shell printf 'icon=1')\n"
                      "$(shell printf 'icon=1'):\n"
                      "\t@printf '%s' '$(VALUE)' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "kept")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "kept")

    def test_call_function_name_is_scoped_across_nested_forwarding(self):
        self.makefile(".DEFAULT_GOAL := all\n0 := outside\n"
                      "inner = $(0):$(1)\n"
                      "wrapper = $(0):$(call inner,$(1)):$(0)\n"
                      "forward_name = $(subst public_,private_,$(1))\n"
                      "private_emit = generated: ; @printf '%s' '$(1)' > generated\n"
                      "public_emit = $(call $(call forward_name,$(0)),$(1))\n"
                      "RESULT := $(call wrapper,value)\n"
                      "$(eval $(call public_emit,forwarded))\n"
                      "all: generated\n\t@printf '%s' '$(RESULT):$(0)' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(),
                         "wrapper:inner:value:wrapper:outside")
        self.assertEqual((self.directory / "generated").read_text(), "forwarded")
        for name in ("result", "generated"):
            (self.directory / name).unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(),
                         "wrapper:inner:value:wrapper:outside")
        self.assertEqual((self.directory / "generated").read_text(), "forwarded")

    def test_eval_macro_folds_dollar_backslash_newline(self):
        self.makefile(
            "toolchains := one\n"
            "tool-classes := cc\n"
            "define configure\n"
            "  $$(foreach class,$$(tool-classes), \\\n"
            "    $$(eval $$(call configure-tool,$1,$$(class))))\n"
            "endef\n"
            "define configure-tool\n"
            "  RESULT.$1.$2 := $$(if yes,$$(or $\\\n"
            "      alpha,beta),fallback)\n"
            "endef\n"
            "$(foreach toolchain,$(toolchains), \\\n"
            "    $(eval $(call configure,$(toolchain))))\n"
            "all:\n\t@test '$(RESULT.one.cc)' = alpha\n"
        )
        self.direct("all")
        self.direct("--ninja", "all")
        self.run_command("sh", "./ninja.sh", "-j2")

    def test_tab_indented_conditional_after_eval_in_else_branch(self):
        self.makefile(
            "ifeq (yes,yes)\n"
            "$(eval GENERATED := yes)\n"
            "else\n"
            "\tifdef OPTIONAL\n"
            "        $(eval NESTED := yes)\n"
            "\tendif\n"
            "endif\n"
            "all:\n\t@test '$(GENERATED)' = yes\n"
        )
        self.direct("all")

    def test_generated_include_enables_another_include_rule(self):
        self.makefile(
            ".DEFAULT_GOAL := all\n"
            "-include dependent.mk\n-include versions.mk\n"
            "ifeq ($(READY),yes)\n"
            "stamp:\n\t@touch $@\nendif\n"
            "dependent.mk: stamp\n\t@printf 'VALUE := ready\\n' > $@\n"
            "versions.mk:\n\t@printf 'READY := yes\\n' > $@\n"
            "all:\n\t@printf '%s' '$(VALUE)' > result\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "ready")
        for name in ("result", "dependent.mk", "versions.mk", "stamp"):
            (self.directory / name).unlink()
        self.direct("--ninja", "--regen", "all")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text(), "ready")

    def test_independent_include_remakes_use_job_limit(self):
        self.makefile(".DEFAULT_GOAL := all\n-include a.mk b.mk\n"
                      "a.mk b.mk:\n\t@python3 includes.py $@\n"
                      "all:\n\t@printf ready > result\n")
        (self.directory / "includes.py").write_text(
            "from pathlib import Path\nimport sys, time\n"
            "Path('start-' + sys.argv[1]).touch()\n"
            "deadline = time.monotonic() + 3\n"
            "while not (Path('start-a.mk').exists() and Path('start-b.mk').exists()):\n"
            "    if time.monotonic() > deadline: raise SystemExit(1)\n"
            "    time.sleep(0.01)\n"
            "Path(sys.argv[1]).write_text('# generated\\n')\n")
        self.direct("-j2", "all")
        self.assertEqual((self.directory / "result").read_text(), "ready")

    def test_secondary_expansion_preserves_function_argument_spaces(self):
        self.makefile(".SECONDEXPANSION:\n"
                      "all: out/first.bau\n"
                      "out/%.bau: $$(addprefix out/$$*/,mimetype data)\n"
                      "\t@cat $^ > result\n"
                      "out/%/mimetype:\n"
                      "\t@mkdir -p $(dir $@); printf mimetype > $@\n"
                      "out/%/data:\n"
                      "\t@mkdir -p $(dir $@); printf data > $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "mimetypedata")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "mimetypedata")

    def test_unchanged_included_makefile_does_not_restart(self):
        (self.directory / "metadata.mk").write_text("VALUE := ok\n")
        self.makefile("include metadata.mk\n.PHONY: force\n"
                      "metadata.mk: force\n\t@printf x >> remakes\n"
                      "all:\n\t@printf '%s' '$(VALUE)' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "ok")
        self.assertEqual((self.directory / "remakes").read_text(), "x")

    def test_touch_does_not_repeatedly_touch_an_included_makefile(self):
        metadata = self.directory / "metadata.mk"
        metadata.write_text("VALUE := ready\n")
        target = self.directory / "goal"
        target.write_text("old\n")
        self.makefile(
            "include metadata.mk\n"
            "metadata.mk:\n\t@printf unexpected > remakes\n"
            "goal:\n\t@printf rebuilt > goal\n"
        )
        before = metadata.stat().st_mtime_ns
        self.direct("--touch", "goal")
        self.assertEqual(metadata.stat().st_mtime_ns, before)
        self.assertFalse((self.directory / "remakes").exists())

    def test_explicit_default_goal(self):
        self.makefile(".DEFAULT_GOAL := all\nwrong:\n\t@echo wrong > result\n"
                      "all:\n\t@echo right > result\n")
        self.direct()
        self.assertEqual((self.directory / "result").read_text().strip(), "right")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text().strip(), "right")

    def test_malformed_conditional_is_diagnosed(self):
        self.makefile("ifeq ($(VALUE,4))\nall:\n\t@echo bad\nendif\n")
        status, output = self.direct("all", success=False)
        self.assertEqual(status, 1, output)
        self.assertIn("invalid syntax", output)

    def test_eval_in_conditional_preserves_preceding_rule(self):
        self.makefile(
            "SOURCE = $(eval SIDE := live)package\n"
            "all:\n"
            "ifneq ($(SOURCE),)\n"
            "\t@printf '%s' '$(SIDE)' > result\n"
            "endif\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "live")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "live")

    def test_rebuilt_child_updates_parent(self):
        self.makefile("all: parent\nparent: child\n\t@cat child > parent\n"
                      "child: source\n\t@cat source > child\n")
        for name, content, stamp in (
            ("source", "new", 300), ("child", "old", 100),
            ("parent", "old", 200),
        ):
            path = self.directory / name
            path.write_text(content)
            os.utime(path, (stamp, stamp))
        self.direct("-j1")
        self.assertEqual((self.directory / "parent").read_text(), "new")

    def test_existing_phony_order_only_is_run(self):
        self.makefile("all: | prepare\n.PHONY: prepare\n"
                      "prepare:\n\t@echo ran > marker\n")
        (self.directory / "prepare").touch()
        self.direct("-j1")
        self.assertEqual((self.directory / "marker").read_text().strip(), "ran")

    def test_dry_run_keeps_intermediate(self):
        self.makefile(".INTERMEDIATE: temp\nall: final\n"
                      "final: temp\n\t@cp temp final\n"
                      "temp:\n\t@echo new > temp\n")
        (self.directory / "temp").write_text("important")
        (self.directory / "final").write_text("important")
        self.direct("-n", "all")
        self.assertEqual((self.directory / "temp").read_text(), "important")

    def test_delete_on_error_direct(self):
        self.makefile(".DELETE_ON_ERROR:\nall:\n"
                      "\t@echo partial > all\n\t@false\n")
        status, _ = self.direct("-j1", success=False)
        self.assertNotEqual(status, 0)
        self.assertFalse((self.directory / "all").exists())

    def test_interruption_cleans_changed_outputs_and_preserves_precious(self):
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport time\n"
            "if Path('arm').exists():\n"
            "    if Path('arm').read_text() == 'change':\n"
            "        Path('result').write_text('partial')\n"
            "    Path('ready').touch()\n"
            "    time.sleep(30)\n"
            "Path('result').write_text(Path('input').read_text())\n")
        for mode in ("direct", "ninja"):
            for policy in ("changed", "unchanged", "precious", "new"):
                with self.subTest(mode=mode, policy=policy):
                    self.makefile(
                        (".PRECIOUS: result\n" if policy == "precious" else "") +
                        "all: result\nresult: input\n\t@python3 worker.py\n")
                    output = self.directory / "result"
                    output.write_text("old")
                    os.utime(output, (time.time() - 10, time.time() - 10))
                    if policy == "new":
                        output.unlink()
                    (self.directory / "input").write_text("complete")
                    (self.directory / "arm").write_text(
                        "nochange" if policy == "unchanged" else "change")
                    ready = self.directory / "ready"
                    ready.unlink(missing_ok=True)
                    if mode == "ninja":
                        self.direct("--ninja", "--regen", "all")
                        command = ["sh", "./ninja.sh", "-j4"]
                    else:
                        command = [str(KATI), "-j4", "all"]
                    with (self.directory / "interrupted.log").open("wb") as log:
                        child = subprocess.Popen(command, cwd=self.directory,
                                                 stdout=log, stderr=log,
                                                 start_new_session=True)
                        try:
                            deadline = time.monotonic() + 10
                            while not ready.exists() and child.poll() is None:
                                self.assertLess(time.monotonic(), deadline)
                                time.sleep(0.02)
                            self.assertTrue(ready.exists())
                            os.killpg(child.pid, signal.SIGINT)
                            self.assertNotEqual(child.wait(timeout=10), 0)
                        finally:
                            if child.poll() is None:
                                os.killpg(child.pid, signal.SIGKILL)
                                child.wait()
                    (self.directory / "arm").unlink()
                    if policy in ("changed", "new"):
                        self.assertFalse(output.exists())
                        if mode == "ninja":
                            self.ninja()
                        else:
                            self.direct("all")
                        self.assertEqual(output.read_text(), "complete")
                    else:
                        self.assertEqual(output.read_text(),
                                         "partial" if policy == "precious" else "old")
                        if policy == "precious":
                            stamp = output.stat().st_mtime_ns
                            if mode == "ninja":
                                self.ninja()
                            else:
                                self.direct("all")
                            self.assertEqual(output.read_text(), "partial")
                            self.assertEqual(output.stat().st_mtime_ns, stamp)

    def test_ninja_cleanup_preserves_recipe_traps_and_signal_exit_status(self):
        for code in (7, 129, 130, 131, 143):
            with self.subTest(code=code):
                self.makefile(
                    ".PHONY: force\nall: result\nresult: force\n"
                    "\t@trap 'printf trapped > trap-result' 0; "
                    f"printf partial > result; exit {code}\n")
                (self.directory / "result").write_text("old")
                self.direct("--ninja", "all")
                status, output = self.run_command(
                    "sh", "./ninja.sh", "-j2", success=False)
                self.assertNotEqual(status, 0, output)
                self.assertEqual((self.directory / "trap-result").read_text(),
                                 "trapped")
                self.assertEqual((self.directory / "result").exists(), code == 7)
                graph = (self.directory / "build.ninja").read_text()
                self.assertNotIn("--internal-output-stamp", graph)
                self.assertNotIn("kati_cleanup_output", graph)

    def test_ninja_cleanup_does_not_arm_before_job_admission(self):
        output = self.directory / "result"
        output.write_text("original")
        fifo = self.directory / "jobserver"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        env = os.environ.copy()
        env["KATI_JOBSERVER_FIFO"] = str(fifo)
        env.pop("KATI_JOBSERVER_RESERVED", None)
        child = subprocess.Popen(
            [str(KATI), "--internal-run-recipe", "/bin/sh", "-c",
             "printf ran > recipe-ran", str(output)], cwd=self.directory,
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            time.sleep(0.1)
            self.assertIsNone(child.poll())
            output.write_text("changed by another process")
            child.send_signal(signal.SIGTERM)
            child.communicate(timeout=5)
            self.assertNotEqual(child.returncode, 0)
            self.assertEqual(output.read_text(), "changed by another process")
            self.assertFalse((self.directory / "recipe-ran").exists())
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()

    def test_ninja_cleanup_handles_quoted_outputs_and_response_files(self):
        for size in (1, 110_000):
            with self.subTest(size=size):
                self.makefile(
                    ".PHONY: force\nall: odd\\ $$\\ name\n"
                    "odd\\ $$\\ name: force\n\t@printf '%s' '" + "x" * size +
                    "' > '$@'; exit 130\n")
                self.direct("--ninja", "all")
                status, output = self.run_command(
                    "sh", "./ninja.sh", "-j2", success=False)
                self.assertNotEqual(status, 0, output)
                self.assertFalse((self.directory / "odd $ name").exists())
                if size > 100_000:
                    self.assertIn(" rspfile =", (self.directory / "build.ninja").read_text())

    def test_ninja_cleanup_reports_interruption_when_recipe_traps_it(self):
        output = self.directory / "odd $ output"
        output.write_text("old")
        command = ("trap 'exit 0' TERM; printf partial > 'odd $ output'; "
                   "printf ready > ready; while :; do sleep 0.05; done")
        child = subprocess.Popen(
            [str(KATI), "--internal-run-recipe", "/bin/sh", "-c", command,
             str(output)], cwd=self.directory, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while not (self.directory / "ready").exists() and child.poll() is None:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            self.assertTrue((self.directory / "ready").exists())
            child.send_signal(signal.SIGTERM)
            child.communicate(timeout=5)
            self.assertEqual(child.returncode, 128 + signal.SIGTERM)
            self.assertFalse(output.exists())
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()

    def test_direct_termination_reaches_recipe_descendants(self):
        self.makefile("all: result\nresult:\n"
                      "\t@echo partial > result; echo ready > ready; sleep 30\n")
        child = subprocess.Popen([str(KATI), "-j4", "all"], cwd=self.directory,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            deadline = time.monotonic() + 10
            while not (self.directory / "ready").exists() and child.poll() is None:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            self.assertTrue((self.directory / "ready").exists())
            child.send_signal(signal.SIGTERM)
            output, _ = child.communicate(timeout=10)
            self.assertNotEqual(child.returncode, 0, output)
            self.assertFalse((self.directory / "result").exists(), output)
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()

    def test_failed_ninja_recipe_stays_failed(self):
        self.makefile("all:\n\t@sleep 0.05; echo partial > all; false\n")
        self.direct("--ninja")
        status, _ = self.run_command("sh", "./ninja.sh", "-j1", success=False)
        self.assertNotEqual(status, 0)

    def test_recipe_signal_is_fatal_even_when_error_ignored(self):
        self.makefile("all: result\nresult:\n"
                      "\t-@echo partial > result; kill -TERM $$$$\n")
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode):
                if mode == "ninja":
                    self.direct("--ninja", "--regen", "all")
                    status, _ = self.run_command("sh", "./ninja.sh", success=False)
                else:
                    status, _ = self.direct("all", success=False)
                self.assertNotEqual(status, 0)
                self.assertFalse((self.directory / "result").exists())

    def test_target_specific_export(self):
        self.makefile("all: export VALUE = target\nall:\n"
                      "\t@printf '%s' \"$${VALUE-unset}\" > result\n")
        self.direct("-j1")
        self.assertEqual((self.directory / "result").read_text(), "target")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "target")

    def test_pattern_specific_export(self):
        self.makefile("LANGS := en-US fr\n%.inc: export COMPLETELANGISO_VAR := $(LANGS)\n"
                      "all: module.inc\nmodule.inc:\n"
                      "\t@printf '%s' \"$${COMPLETELANGISO_VAR-unset}\" > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "en-US fr")

    def test_long_direct_recipe_bypasses_single_argument_limit(self):
        payload = "x" * 140_000
        self.makefile("all:\n\t@printf '%s' '" + payload + "' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), payload)

    def test_existing_directory_target_does_not_rerun_mkdir(self):
        self.makefile("all: lib\nlib:\n\t@mkdir lib\n")
        self.direct("all")
        self.direct("all")
        self.assertTrue((self.directory / "lib").is_dir())

    def test_double_colon_recipes_use_independent_freshness(self):
        self.makefile("a:: b\n\t@printf rule1 >> a\n"
                      "a:: c\n\t@printf rule2 >> a\n")
        for name in ("a", "b", "c"):
            (self.directory / name).write_text("")
        os.utime(self.directory / "a", (200, 200))
        os.utime(self.directory / "b", (100, 100))
        os.utime(self.directory / "c", (100, 100))
        self.direct("a")
        self.assertEqual((self.directory / "a").read_text(), "")
        os.utime(self.directory / "b", (300, 300))
        self.direct("a")
        self.assertEqual((self.directory / "a").read_text(), "rule1")
        (self.directory / "a").write_text("")
        os.utime(self.directory / "a", (200, 200))
        os.utime(self.directory / "b", (100, 100))
        os.utime(self.directory / "c", (300, 300))
        self.direct("a")
        self.assertEqual((self.directory / "a").read_text(), "rule2")

    def test_shell_function_stops_at_nul_without_truncating_recipe(self):
        (self.directory / "loader").write_bytes(b"/lib/loader.so\0")
        self.makefile("all:\n"
                      "\t@printf '%s' '$(shell cat loader)' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(),
                         "/lib/loader.so")

    def test_oneshell_conditional_and_prefixes(self):
        self.makefile(".ONESHELL:\nall:\n\t@if true; then\n"
                      "\t  echo yes > result\n\t@fi\n")
        self.direct("-j1")
        self.assertEqual((self.directory / "result").read_text().strip(), "yes")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text().strip(), "yes")

    def test_existing_ninja_leaf_can_be_rebuilt(self):
        self.makefile("all: leaf\nleaf:\n\t@echo hi > leaf\n")
        (self.directory / "leaf").write_text("old")
        self.direct("--ninja")
        (self.directory / "leaf").unlink()
        self.run_command("sh", "./ninja.sh", "-j1")
        self.assertEqual((self.directory / "leaf").read_text().strip(), "hi")

    def test_existing_output_with_order_only_prerequisite_stays_current(self):
        self.makefile(
            "all: output\n"
            "output: | prepare\n\t@printf 'rebuilt' > output\n"
            ".PHONY: prepare\n"
            "prepare:\n\t@printf 'ready' > marker\n"
        )
        (self.directory / "output").write_text("current")
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "marker").read_text(), "ready")
        self.assertEqual((self.directory / "output").read_text(), "current")
        (self.directory / "output").unlink()
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "output").read_text(), "rebuilt")

    def test_included_makefile_with_directory_prerequisite(self):
        self.makefile(
            "include build/version.mk\n"
            "all: result\n"
            "result:\n\t@printf '%s\\n' '$(version)' > $@\n"
            "build/version.mk: | build\n"
            "\t@printf 'version := ready\\n' > $@\n"
            "build:\n\t@mkdir -p $@\n"
        )
        self.direct("--ninja", "--regen")
        self.assertTrue((self.directory / "build.ninja").exists())
        self.assertEqual(
            (self.directory / "build/version.mk").read_text().strip(),
            "version := ready",
        )
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text().strip(), "ready")
        self.direct("--ninja", "--regen")

    def test_missing_include_does_not_invent_parent_directory_cycle(self):
        self.makefile(
            "-include include/config/auto.conf\n"
            "all: output\n"
            "output:\n\t@printf ready > $@\n"
            "include/config/auto.conf: config\n"
            "\t@mkdir -p include/config\n"
            "\t@printf 'ready := yes\\n' > $@\n"
            "config:\n\t@touch $@\n"
            "%/: prepare\n\t@mkdir -p $@\n"
            "prepare: include/config/auto.conf\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "output").read_text(), "ready")
        self.assertTrue((self.directory / "include/config/auto.conf").exists())

    def test_suffix_reset_keeps_first_makefile_goal_as_default(self):
        self.makefile(
            ".SUFFIXES:\n"
            "all: result\n"
            "result:\n\t@printf 'ready\\n' > $@\n"
        )
        self.direct("--ninja")
        self.assertIn("default all\n", (self.directory / "build.ninja").read_text())
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text().strip(), "ready")

    def test_ninja_directory_targets_create_outputs(self):
        self.makefile(
            "all: new/result existing/result\n"
            "new/result existing/result: %/result: | %/\n"
            "\t@printf 'ready\\n' > $@\n"
            "new/ existing/: \n\t@mkdir -p $@\n"
        )
        (self.directory / "existing").mkdir()
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        for name in ("new", "existing"):
            self.assertEqual(
                (self.directory / name / "result").read_text().strip(),
                "ready",
            )

    def test_existing_directory_alias_does_not_duplicate_ninja_output(self):
        self.makefile(
            "all: doc doc/result\n"
            "doc:\n\t@:\n"
            "doc/result: | doc/\n\t@printf 'ready\\n' > $@\n"
            "doc/: \n\t@mkdir -p $@\n"
        )
        (self.directory / "doc").mkdir()
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "doc/result").read_text().strip(), "ready")

    def test_existing_source_from_implicit_rule_is_not_cleaned(self):
        self.makefile(
            ".SUFFIXES:\n"
            "all: source.o\n"
            "%.o: %.c %.h\n\t@cp $< $@\n"
            "%.h:\n\t@:\n"
        )
        (self.directory / "source.c").write_text("source")
        (self.directory / "source.h").write_text("keep this header")
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "source.o").read_text(), "source")
        self.assertEqual(
            (self.directory / "source.h").read_text(), "keep this header"
        )
        self.run_command("sh", "./ninja.sh", "-t", "clean")
        self.assertEqual(
            (self.directory / "source.h").read_text(), "keep this header"
        )
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "source.o").read_text(), "source")

    def test_ninja_respects_current_output_before_first_build_log_entry(self):
        self.makefile(
            "all: generated\n"
            "generated: source\n\t@printf 'rebuilt' > $@\n"
        )
        (self.directory / "source").write_text("input")
        (self.directory / "generated").write_text("current")
        os.utime(self.directory / "source", (100, 100))
        os.utime(self.directory / "generated", (200, 200))
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "generated").read_text(), "current")
        os.utime(self.directory / "source", (300, 300))
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "generated").read_text(), "rebuilt")

    def test_existing_file_with_rule_has_one_ninja_output_for_path_aliases(self):
        self.makefile(
            "all: result\n"
            "result: one/../source.h two/../source.h\n"
            "\t@cp source.h $@\n"
            "source.h:\n\t@:\n"
        )
        (self.directory / "one").mkdir()
        (self.directory / "two").mkdir()
        (self.directory / "source.h").write_text("keep this header")
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "result").read_text(), "keep this header")
        self.assertEqual(
            (self.directory / "source.h").read_text(), "keep this header"
        )

    def test_ninja_executes_large_recipe_from_response_file(self):
        payload = "x" * 110_000
        self.makefile(
            "all: out\n"
            "out:\n\t@printf '%s' '" + payload + "' > $@\n"
        )
        self.direct("--ninja")
        self.run_command("sh", "./ninja.sh", "-j2")
        self.assertEqual((self.directory / "out").read_text(), payload)

    def test_regen_detects_missing_support_and_read_inputs(self):
        self.makefile("V := $(file <input)\nall:\n\t@echo $(V)\n")
        (self.directory / "input").write_text("hello")
        self.direct("--ninja", "--regen")
        (self.directory / "input").unlink()
        _, output = self.direct("--ninja", "--regen")
        self.assertNotIn("No need to regenerate", output)
        (self.directory / "env.sh").unlink()
        _, output = self.direct("--ninja", "--regen")
        self.assertNotIn("No need to regenerate", output)

    def test_corrupt_stamp_regenerates(self):
        self.makefile("all:\n\t@echo ok\n")
        self.direct("--ninja", "--regen")
        (self.directory / ".kati_stamp").write_bytes(
            b"\0" * 8 + b"KAT2" + b"\xff" * 4
        )
        status, output = self.direct("--ninja", "--regen", success=False)
        self.assertEqual(status, 0, output)
        self.assertNotIn("No need to regenerate", output)

    def test_missing_option_values_fail_cleanly(self):
        self.makefile("all:\n\t@true\n")
        for option in ("--remote_num_jobs", "--writable"):
            with self.subTest(option=option):
                status, output = self.direct(option, success=False)
                self.assertNotEqual(status, 0)
                self.assertGreaterEqual(status, 0, output)

    def test_parallel_missing_prerequisites_fail_cleanly(self):
        self.makefile("all: foo bar\n")
        for _ in range(10):
            status, output = self.direct("-j4", success=False)
            self.assertEqual(status, 1, output)
            self.assertIn("No rule to make target", output)

    def test_question_mode_missing_prerequisite_reports_failure(self):
        self.makefile("all: absent\n")
        status, output = self.direct("-q", "all", success=False)
        self.assertEqual(status, 2, output)
        self.assertIn("No rule to make target", output)

    def test_question_mode_empty_rule_needs_no_recipe(self):
        self.makefile("all: absent\nabsent:\n")
        status, output = self.direct("-q", "all", success=False)
        self.assertEqual(status, 0, output)

    def test_job_limit_and_nested_parallelism(self):
        self.makefile("all: group\ngroup: a b\na b:\n"
                      "\t@python3 worker.py $@\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport sys, time\n"
            "Path('start-' + sys.argv[1]).touch()\n"
            "deadline = time.monotonic() + 2\n"
            "while not (Path('start-a').exists() and Path('start-b').exists()):\n"
            "    if time.monotonic() > deadline: raise SystemExit(1)\n"
            "    time.sleep(0.01)\n"
            "Path(sys.argv[1]).touch()\n", encoding="utf-8",
        )
        self.direct("-j2")
        self.assertTrue((self.directory / "a").exists())
        self.assertTrue((self.directory / "b").exists())
        for name in ("a", "b", "start-a", "start-b"):
            (self.directory / name).unlink()
        self.ninja()
        self.assertTrue((self.directory / "a").exists())
        self.assertTrue((self.directory / "b").exists())

    def test_nested_kati_in_recipe_uses_reserved_jobserver_slot(self):
        self.makefile("all:\n\t@python3 nested.py\n")
        (self.directory / "child.mk").write_text(
            '.PHONY: all\nall:\n\t@"$(MAKE)" -f grandchild.mk -j1 all\n',
            encoding="utf-8",
        )
        (self.directory / "grandchild.mk").write_text(
            ".PHONY: all\nall:\n\t@printf x >> done\n",
            encoding="utf-8",
        )
        (self.directory / "nested.py").write_text(
            "import subprocess\n"
            f"for _ in range(2):\n    subprocess.run([{str(KATI)!r}, "
            "'-f', 'child.mk', '-j1', 'all'], check=True)\n",
            encoding="utf-8",
        )
        fifo = self.directory / "jobserver"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        os.write(fd, b"x")
        env = os.environ.copy()
        env["KATI_JOBSERVER_FIFO"] = str(fifo)
        env["KATI_JOBS"] = "1"
        env.pop("KATI_JOBSERVER_RESERVED", None)
        result = subprocess.run(
            [str(KATI), "-j1", "all"], cwd=self.directory, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=5, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual((self.directory / "done").read_text(), "xx")

    def test_recursive_siblings_share_jobserver_concurrently(self):
        self.makefile('all:\n\t@"$(MAKE)" -f children.mk all\n')
        (self.directory / "children.mk").write_text(
            'all: a b\na b:\n\t@"$(MAKE)" -f leaf.mk NAME=$@ all\n')
        (self.directory / "leaf.mk").write_text(
            "all:\n\t@python3 worker.py $(NAME)\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport sys, time\n"
            "Path('start-' + sys.argv[1]).touch()\n"
            "deadline = time.monotonic() + 2\n"
            "while not (Path('start-a').exists() and Path('start-b').exists()):\n"
            "    if time.monotonic() > deadline: raise SystemExit(1)\n"
            "    time.sleep(0.01)\n")
        fifo = self.directory / "jobserver"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        os.write(fd, b"xx")
        env = os.environ.copy()
        env["KATI_JOBSERVER_FIFO"] = str(fifo)
        env["KATI_JOBS"] = "2"
        env.pop("KATI_JOBSERVER_RESERVED", None)
        for mode in ("direct", "ninja"):
            with self.subTest(mode=mode):
                for name in ("start-a", "start-b"):
                    (self.directory / name).unlink(missing_ok=True)
                if mode == "ninja":
                    self.direct("--ninja", "-j2", "all")
                    command = ["sh", "./ninja.sh", "-j2"]
                else:
                    command = [str(KATI), "-j2", "all"]
                result = subprocess.run(
                    command, cwd=self.directory, env=env, capture_output=True,
                    timeout=10, check=False)
                self.assertEqual(result.returncode, 0,
                                 result.stdout.decode() + result.stderr.decode())
                self.assertEqual(os.read(fd, 3), b"xx")
                os.write(fd, b"xx")

    def test_recursive_siblings_serialize_without_usable_jobserver(self):
        self.makefile('all:\n\t@"$(MAKE)" -f children.mk all\n')
        (self.directory / "children.mk").write_text(
            'all: a b\na b:\n\t@"$(MAKE)" -f leaf.mk all\n')
        (self.directory / "leaf.mk").write_text(
            "all:\n\t@python3 worker.py\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport time\n"
            "lock = Path('lock')\nlock.mkdir()\n"
            "time.sleep(0.15)\nlock.rmdir()\n"
            "with Path('done').open('a') as f: f.write('x')\n")
        missing = self.directory / "missing-fifo"
        deleted = self.directory / "deleted-fifo"
        os.mkfifo(deleted)
        deleted.unlink()
        regular = self.directory / "regular-file"
        regular.write_bytes(b"xx")
        directory = self.directory / "directory"
        directory.mkdir()
        dangling = self.directory / "dangling-link"
        dangling.symlink_to(missing)
        env = os.environ.copy()
        env["KATI_JOBS"] = "2"
        env.pop("KATI_JOBSERVER_RESERVED", None)
        for mode in ("direct", "ninja"):
            if mode == "ninja":
                self.direct("--ninja", "-j2", "all")
                command = ["sh", "./ninja.sh", "-j2"]
            else:
                command = [str(KATI), "-j2", "all"]
            for fifo in (missing, deleted, regular, directory, dangling):
                with self.subTest(mode=mode, fifo=fifo.name):
                    (self.directory / "done").unlink(missing_ok=True)
                    env["KATI_JOBSERVER_FIFO"] = str(fifo)
                    result = subprocess.run(
                        command, cwd=self.directory, env=env, capture_output=True,
                        timeout=10, check=False)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout.decode() + result.stderr.decode())
                    self.assertEqual((self.directory / "done").read_text(), "xx")
                    self.assertEqual(regular.read_bytes(), b"xx")

    def test_noop_recipe_does_not_consume_job_token(self):
        self.makefile("all:\n\t@:\n")
        fifo = self.directory / "jobserver"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        env = os.environ.copy()
        env["KATI_JOBSERVER_FIFO"] = str(fifo)
        env.pop("KATI_JOBSERVER_RESERVED", None)
        result = subprocess.run(
            [str(KATI), "-j1", "all"], cwd=self.directory, env=env,
            capture_output=True, timeout=3, check=False)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with self.assertRaises(BlockingIOError):
            os.read(fd, 1)

    def test_noop_custom_interpreters_and_flags_still_run(self):
        shell = self.directory / "recipe-shell"
        shell.write_text("#!/bin/sh\nprintf x >> shell-used\n"
                         'exec /bin/sh "$@"\n', encoding="utf-8")
        shell.chmod(0o755)
        self.makefile(f"SHELL := {shell}\nall:\n\t@:\n")
        self.direct("all")
        self.assertEqual((self.directory / "shell-used").read_text(), "x")
        self.makefile(".SHELLFLAGS := -xc\nall:\n\t@:\n")
        _, output = self.direct("all")
        self.assertIn("+ :", output)

    def test_recursive_siblings_respect_shared_job_budget(self):
        self.makefile('.EXPORT_ALL_VARIABLES:\nexport KATI_JOBSERVER_RESERVED\n'
                      'all:\n\t@"$(MAKE)" -f children.mk all\n')
        (self.directory / "children.mk").write_text(
            '.EXPORT_ALL_VARIABLES:\nexport KATI_JOBSERVER_RESERVED\n'
            'all: a b c d\na b c d:\n\t@"$(MAKE)" -f leaf.mk all\n')
        (self.directory / "leaf.mk").write_text(
            "all: w x y z\nw x y z:\n\t@python3 worker.py\n")
        (self.directory / "worker.py").write_text(
            "import fcntl, json, time\n"
            "def update(delta):\n"
            "    with open('counts', 'r+') as f:\n"
            "        fcntl.flock(f, fcntl.LOCK_EX)\n"
            "        active, peak, done = json.load(f)\n"
            "        active += delta\n"
            "        f.seek(0)\n"
            "        json.dump([active, max(peak, active), done + (delta < 0)], f)\n"
            "        f.truncate()\n"
            "update(1)\ntime.sleep(0.04)\nupdate(-1)\n")
        fifo = self.directory / "jobserver"
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        os.write(fd, b"xx")
        env = os.environ.copy()
        env["KATI_JOBSERVER_FIFO"] = str(fifo)
        env["KATI_JOBS"] = "4"
        env.pop("KATI_JOBSERVER_RESERVED", None)
        for mode, reserved in (("direct", False), ("ninja", False),
                               ("direct", True), ("ninja", True)):
            with self.subTest(mode=mode, reserved=reserved):
                self.assertEqual(os.read(fd, 3), b"xx")
                tokens = b"x" if reserved else b"xx"
                os.write(fd, tokens)
                if reserved:
                    env["KATI_JOBSERVER_RESERVED"] = "1"
                else:
                    env.pop("KATI_JOBSERVER_RESERVED", None)
                (self.directory / "counts").write_text("[0, 0, 0]")
                if mode == "ninja":
                    self.direct("--ninja", "-j4", "all")
                    command = ["sh", "./ninja.sh", "-j4"]
                else:
                    command = [str(KATI), "-j4", "all"]
                result = subprocess.run(
                    command, cwd=self.directory, env=env, capture_output=True,
                    timeout=10, check=False)
                self.assertEqual(result.returncode, 0,
                                 result.stdout.decode() + result.stderr.decode())
                self.assertEqual(json.loads((self.directory / "counts").read_text()),
                                 [0, 2, 16])
                self.assertEqual(os.read(fd, 3), tokens)
                os.write(fd, b"xx")

    def test_recursive_make_inherits_job_limit(self):
        self.makefile('all:\n\t@"$(MAKE)" -f child.mk all\n')
        (self.directory / "child.mk").write_text(
            "all: a b\na b:\n\t@python3 worker.py\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport time\n"
            "lock = Path('lock')\nlock.mkdir()\n"
            "time.sleep(0.1)\nlock.rmdir()\n"
            "with Path('done').open('a') as output: output.write('x')\n")
        self.direct("-j1")
        self.assertEqual((self.directory / "done").read_text(), "xx")

    def test_recursive_make_inherits_dry_run(self):
        self.makefile(".PHONY: all child\n"
                      "all:\n\t$(MAKE) child\n"
                      "child:\n\t@printf ran > child-output\n")
        self.direct("-n", "MAKE=" + str(KATI))
        self.assertFalse((self.directory / "child-output").exists())

        inherited = subprocess.run(
            [str(KATI), "child"], cwd=self.directory,
            env={**os.environ, "MAKEFLAGS": "-n -j1"},
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=15, check=False,
        )
        self.assertEqual(inherited.returncode, 0, inherited.stdout.decode())
        self.assertFalse((self.directory / "child-output").exists())

    def test_explicit_prerequisite_preserves_implicit_generated_source(self):
        rules = (".SUFFIXES: .l .c .o\n"
                 "all: sample.o\n"
                 ".l.c:\n\t@cp $< $@\n"
                 ".c.o:\n\t@cp $< $@\n")
        (self.directory / "sample.l").write_text("lexer source\n")
        self.makefile(rules + "dist: sample.c\n")
        self.direct("all")
        self.assertTrue((self.directory / "sample.c").exists())
        self.assertEqual((self.directory / "sample.o").read_text(),
                         "lexer source\n")

        (self.directory / "sample.c").unlink()
        (self.directory / "sample.o").unlink()
        self.makefile(rules)
        self.direct("all")
        self.assertFalse((self.directory / "sample.c").exists())

    def test_suffix_rule_definition_order(self):
        self.makefile(".SUFFIXES: .adb .ads .o\n"
                      "all: sample.o\n"
                      ".adb.o:\n\t@cp $< $@\n"
                      ".ads.o:\n\t@cp $< $@\n")
        (self.directory / "sample.adb").write_text("body\n")
        (self.directory / "sample.ads").write_text("spec\n")
        self.direct("all")
        self.assertEqual((self.directory / "sample.o").read_text(), "body\n")

    def test_builtin_fortran_suffix_rule(self):
        self.makefile(".SUFFIXES: .F .f90 .F90 .o .mod\nall: sample.o\n")
        (self.directory / "sample.f").write_text(
            "      subroutine sample()\n      end\n")
        (self.directory / "fake-fc.sh").write_text(
            "#!/bin/sh\nwhile test $# -gt 0; do\n"
            "  if test \"$1\" = -o; then shift; out=$1; fi\n"
            "  shift\ndone\nprintf compiled > \"$out\"\n")
        self.direct("FC=sh fake-fc.sh", "all")
        self.assertEqual((self.directory / "sample.o").read_text(), "compiled")

    def test_declared_multidot_suffix_rule(self):
        self.makefile(".SUFFIXES:\n.SUFFIXES: .log .test.bin\n"
                      "all: sample.log\n"
                      "sample.log: sample.test.bin\n"
                      ".test.bin.log:\n\t@cp $< $@\n")
        (self.directory / "sample.test.bin").write_text("test result")
        self.direct("all")
        self.assertEqual((self.directory / "sample.log").read_text(),
                         "test result")

    def test_verbose_environment_prints_recipe(self):
        self.makefile("all:\n\tprintf verbose > result\n")
        env = dict(os.environ, KATI_VERBOSE="1")
        result = subprocess.run(
            [str(KATI), "all"], cwd=self.directory, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=15, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn("printf verbose > result", result.stdout.decode())

    def test_verbose_environment_omits_noop_diagnostic(self):
        self.makefile("all: existing\n")
        (self.directory / "existing").touch()
        env = dict(os.environ, KATI_VERBOSE="1")
        result = subprocess.run(
            [str(KATI), "all"], cwd=self.directory, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=15, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(result.stdout, b"")

    def test_make_compatible_print_directory_and_unbounded_jobs_flags(self):
        self.makefile("all:\n\t@printf built > result\n")
        self.direct("-w", "-j", "all")
        self.assertEqual((self.directory / "result").read_text(), "built")

    def test_silent_noop_has_no_diagnostic_output(self):
        self.makefile("all: existing\n")
        (self.directory / "existing").touch()
        _, output = self.direct("-s", "all")
        self.assertEqual(output, "")

    def test_plus_recipe_executes_during_dry_run(self):
        self.makefile("all:\n\t@printf skipped > skipped\n"
                      "\t+@printf executed > executed\n")
        self.direct("-n", "all")
        self.assertFalse((self.directory / "skipped").exists())
        self.assertEqual((self.directory / "executed").read_text(),
                         "executed")

    def test_abbreviated_include_dir_option(self):
        self.makefile("all:\n\t@printf ok > result\n")
        self.direct("--include", "unused", "all")
        self.assertEqual((self.directory / "result").read_text(), "ok")

    def test_hash_in_command_line_variable_is_literal(self):
        self.makefile("all:\n\t@printf '%s' '$(VALUE)' > result\n")
        self.direct("VALUE=a#b", "all")
        self.assertEqual((self.directory / "result").read_text(), "a#b")

    def test_hash_in_command_line_variable_survives_recursion(self):
        self.makefile("all:\n\t@printf '%s' '$(VALUE)' > parent\n"
                      "\t@printf '%s' \"$$MAKEFLAGS\" > flags\n"
                      "\t@$(MAKE) -s child\n"
                      "child:\n\t@printf '%s' '$(VALUE)' > result\n")
        self.direct("VALUE=# ", "all")
        self.assertEqual((self.directory / "result").read_text(), "# ",
                         ((self.directory / "parent").read_text(),
                          (self.directory / "flags").read_text()))

    def test_primary_makefile_is_remade_before_goals(self):
        def content(value):
            return (f"VALUE = {value}\n"
                    "all:\n\t@printf '%s' '$(VALUE)' > result\n"
                    "Makefile: template\n\t@cp template Makefile\n")

        self.makefile(content("stale"))
        (self.directory / "template").write_text(content("refreshed"))
        makefile = self.directory / "Makefile"
        old = makefile.stat().st_mtime - 10
        os.utime(makefile, (old, old))
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "refreshed")

    def test_vpath_explicit_generated_source_uses_current_provider(self):
        source = self.directory / "src"
        source.mkdir()
        generated = source / "generated.c"
        prerequisite = source / "input.y"
        generated.write_text("release source\n")
        prerequisite.write_text("regenerated source\n")
        now = generated.stat().st_mtime
        os.utime(generated, (now - 10, now - 10))
        os.utime(prerequisite, (now - 20, now - 20))
        self.makefile("VPATH = src\n"
                      "all: output\n"
                      "output: generated.c\n\t@cp $< $@\n"
                      "generated.c: input.y\n\t@cp $< $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "output").read_text(),
                         "release source\n")
        self.assertFalse((self.directory / "generated.c").exists())

        (self.directory / "output").unlink()
        os.utime(prerequisite, (now - 5, now - 5))
        self.direct("all")
        self.assertEqual((self.directory / "output").read_text(),
                         "regenerated source\n")
        self.assertTrue((self.directory / "generated.c").exists())

    def test_verbose_make_variable_prints_recipe(self):
        self.makefile("all:\n\tprintf verbose > result\n")
        _, output = self.direct("V=1")
        self.assertIn("printf verbose > result", output)
        self.assertNotIn("BUILD   all", output)
        self.assertEqual((self.directory / "result").read_text(), "verbose")

    def test_wrapper_receives_effective_job_limit(self):
        self.makefile("all:\n\t@printf '%s' '$(filter -j%,$(MAKEFLAGS))' > jobs\n")
        self.direct("-j4")
        self.assertEqual((self.directory / "jobs").read_text(), "-j4")
        self.makefile("MAKEFLAGS += -j8\n"
                      "all:\n\t@printf '%s' '$(filter -j%,$(MAKEFLAGS))' > jobs\n")
        self.direct("-j2")
        self.assertEqual((self.directory / "jobs").read_text(), "-j2")

    def test_recursion_level_gates_top_level_generated_headers(self):
        self.makefile("ifeq ($(MAKELEVEL),0)\n"
                      "all: header\nendif\n"
                      "all:\n\t@printf '%s/%s\\n' '$(MAKELEVEL)' \"$$MAKELEVEL\" >> levels\n"
                      "\t@\"$(MAKE)\" -f child.mk\n"
                      "header:\n\t@printf ready > header\n")
        (self.directory / "child.mk").write_text(
            "all:\n\t@test -f header\n"
            "\t@printf '%s/%s\\n' '$(MAKELEVEL)' \"$$MAKELEVEL\" >> levels\n")
        self.direct("all")
        self.assertEqual((self.directory / "levels").read_text(), "0/1\n1/2\n")
        (self.directory / "levels").unlink()
        (self.directory / "header").unlink()
        self.ninja()
        self.assertEqual((self.directory / "levels").read_text(), "0/1\n1/2\n")

    def test_appended_vpath_finds_implicit_sources(self):
        for name in ("first", "second"):
            (self.directory / name).mkdir()
        (self.directory / "first/a.c").write_text("first")
        (self.directory / "second/b.c").write_text("second")
        self.makefile("VPATH = first\nVPATH += second\n"
                      "all: a.o b.o\n%.o: %.c\n\t@cp $< $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "a.o").read_text(), "first")
        self.assertEqual((self.directory / "b.o").read_text(), "second")
        (self.directory / "a.o").unlink()
        (self.directory / "b.o").unlink()
        self.ninja()
        self.assertEqual((self.directory / "a.o").read_text(), "first")
        self.assertEqual((self.directory / "b.o").read_text(), "second")

    def test_added_suffix_preserves_default_compilation_rule(self):
        source = self.directory / "src"
        source.mkdir()
        (source / "fingerprint.c").write_text("source")
        self.makefile("VPATH = src\n.SUFFIXES: .c\n"
                      "all: fingerprint.o\n"
                      "fingerprint.o: header\n"
                      "header:\n\t@touch $@\n"
                      ".c.o:\n\t@cp $< $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "fingerprint.o").read_text(),
                         "source")

    def test_parent_vpath_suffix_source_survives_normalization(self):
        (self.directory / "foo.c").write_text("source")
        build = self.directory / "work" / "inner"
        build.mkdir(parents=True)
        (build / "Makefile").write_text(
            "VPATH = ../..\n.SUFFIXES:\n.SUFFIXES: .c .o\n"
            "all: foo.o\n.c.o:\n\t@cp $< $@\n"
        )
        result = subprocess.run(
            [str(KATI), "all"], cwd=build, capture_output=True,
            timeout=15, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual((build / "foo.o").read_text(), "source")

    def test_direct_vpath_target_uses_existing_source_rule(self):
        source = self.directory / "src"
        source.mkdir()
        (source / "m4.1").write_text("manual")
        self.makefile(f"VPATH = {source}\n"
                      f"srcdir = {source}\n"
                      "$(srcdir)/m4.1:\n")
        self.direct("m4.1")
        self.assertEqual((source / "m4.1").read_text(), "manual")

    def test_vpath_source_with_explicit_generation_rule(self):
        source = self.directory / "src"
        source.mkdir()
        (source / "foo.c").write_text("original")
        self.makefile("VPATH = src\nall: foo.o\n"
                      "foo.c:\n\t@printf regenerated > foo.c\n"
                      "foo.o: foo.c\n\t@cp $< $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "foo.o").read_text(), "original")
        self.assertFalse((self.directory / "foo.c").exists())

    def test_recursive_vpath_expands_after_late_srcdir_assignment(self):
        source = self.directory / "source"
        source.mkdir()
        (source / "input.txt").write_text("found")
        self.makefile("VPATH = VPATH=.:${srcdir}\n"
                      "srcdir = source\n"
                      "all: output\n"
                      "output: input.txt\n\t@cp $< $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "output").read_text(), "found")

    def test_define_header_assignment_operator_keeps_recipe_name(self):
        self.makefile("define zip_recipe =\n"
                      "@printf 'made' > $@\n"
                      "endef\n"
                      "all: archive.ott\n"
                      "archive.ott:\n\t$(zip_recipe)\n")
        self.direct("all")
        self.assertEqual((self.directory / "archive.ott").read_text(), "made")

    def test_dotless_suffix_chain(self):
        (self.directory / "fooa").write_text("source")
        self.makefile(".SUFFIXES:\n.SUFFIXES: a b c .oOo\n"
                      "ab:\n\t@printf 'ab:' > $@; cat $< >> $@\n"
                      "bc:\n\t@printf 'bc:' > $@; cat $< >> $@\n"
                      "c.oOo:\n\t@printf 'object:' > $@; cat $< >> $@\n"
                      "all: foo.oOo\n")
        self.direct("all")
        self.assertEqual((self.directory / "foo.oOo").read_text(),
                         "object:bc:ab:source")
        self.assertFalse((self.directory / "foob").exists())
        self.assertFalse((self.directory / "fooc").exists())
        self.direct("all")
        self.assertFalse((self.directory / "foob").exists())
        self.assertFalse((self.directory / "fooc").exists())
        source = self.directory / "fooa"
        source.write_text("changed")
        future = (self.directory / "foo.oOo").stat().st_mtime + 2
        os.utime(source, (future, future))
        self.direct("all")
        self.assertEqual((self.directory / "foo.oOo").read_text(),
                         "object:bc:ab:changed")

    def test_unused_unterminated_recursive_value_is_deferred(self):
        self.makefile("BROKEN = $(shell echo never-used\n"
                      "all:\n\t@printf pass > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "pass")

    def test_newer_inputs_use_target_age_before_prerequisite_recipes(self):
        self.makefile("all: out\nout: dep\n"
                      "\t@printf '%s' '$?' > trace\n"
                      "dep: input\n\t@touch dep out\n")
        for name, age in (("out", 100), ("dep", 100), ("input", 200)):
            path = self.directory / name
            path.touch()
            os.utime(path, (age, age))
        self.direct("all")
        self.assertEqual((self.directory / "trace").read_text(), "dep")

    def test_redundant_curdir_slash_remains_relative(self):
        (self.directory / "ltmain.sh").write_text("present")
        self.makefile("all: .//ltmain.sh\n\t@cat $< > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "present")

    def test_missing_empty_rule_rebuilds_dependents(self):
        self.makefile("all: result\nresult: absent\n"
                      "\t@printf x >> trace\n\t@touch result\n"
                      "absent:\n")
        self.direct("all")
        self.direct("all")
        self.assertEqual((self.directory / "trace").read_text(), "xx")

    def test_missing_recipe_target_rebuilds_dependents(self):
        self.makefile("all: result\nresult: force\n"
                      "\t@printf x >> trace\n\t@touch result\n"
                      "force:\n\t@:\n")
        self.direct("all")
        self.direct("all")
        self.assertEqual((self.directory / "trace").read_text(), "xx")

    def test_mflags_exports_recursive_options_without_overrides(self):
        self.makefile("all:\n\t@printf '%s\\n%s' \"$$MFLAGS\" "
                      "\"$$MAKEFLAGS\" > result\n")
        self.direct("-k", "-j1", "FOO=bar", "all")
        mflags, makeflags = (self.directory / "result").read_text().splitlines()
        self.assertIn("-k", mflags)
        self.assertIn("-j1", mflags)
        self.assertNotIn("FOO=bar", mflags)
        self.assertIn("FOO=bar", makeflags)

    def test_recursive_command_line_append_stays_deferred(self):
        self.makefile(
            "all:\n"
            "\t@$(MAKE) -s child\n"
            "child: PART := child\n"
            "child:\n"
            "\t@printf '%s|%s' '$(origin LIBS)' '$(LIBS)' > result\n"
        )
        self.direct("-s", "LIBS+=$(if $(filter child,$(PART)),found)", "all")
        self.assertEqual((self.directory / "result").read_text(),
                         "command line|found")

    def test_vpath_keeps_source_with_newer_header_and_no_recipe(self):
        source = self.directory / "source"
        source.mkdir()
        (source / "item.c").write_text("source")
        (source / "header.h").write_text("header")
        os.utime(source / "item.c", (100, 100))
        os.utime(source / "header.h", (200, 200))
        self.makefile(
            "VPATH = source\n"
            "item.c: header.h\n"
            "item.o: item.c\n"
            "\t@cp $< $@\n"
        )
        self.direct("item.o")
        self.assertEqual((self.directory / "item.o").read_text(), "source")

    def test_empty_eval_line_does_not_capture_following_recipe(self):
        (self.directory / "input").write_text("source")
        self.makefile(
            "define generated\n"
            "out: input\n"
            "\t@cp $$< $$@\n"
            "endef\n"
            "$(eval $(generated))\n"
            "# commented-out rule:\n"
            "\t# echo ignored > $@\n"
        )
        self.direct("out")
        self.assertEqual((self.directory / "out").read_text(), "source")

    def test_implicit_rule_skips_self_referential_directory_stamp(self):
        self.makefile(
            "all: work/thing/data\n"
            "work/thing/%: | work/thing/.dir\n"
            "\t@printf ready > $@\n"
            "work/%/.dir:\n"
            "\t@mkdir -p $(@D); touch $@\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "work/thing/data").read_text(),
                         "ready")

    def test_recipe_eval_appends_to_global_from_target_scope(self):
        self.makefile(
            "FLAGS := base\n"
            "all: LOCAL := target\n"
            "all:\n"
            "\t@$(eval FLAGS += extra)\n"
            "\t@printf '%s' '$(FLAGS)' > result\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(),
                         "base extra")

    def test_recipe_less_pattern_does_not_hide_link_rule(self):
        (self.directory / "program.c").write_text("source")
        self.makefile("all: program\nprogram: program.o common.o\n"
                      "%: %.c\n%: %.o\n\t@cat $^ > $@\n"
                      "%.o: %.c\n\t@cp $< $@\n"
                      "common.o:\n\t@printf common > $@\n")
        self.direct("all")
        self.assertEqual((self.directory / "program").read_text(), "sourcecommon")
        (self.directory / "program").unlink()
        self.ninja()
        self.assertEqual((self.directory / "program").read_text(), "sourcecommon")

    def test_recipe_less_pattern_chains_generated_source_to_compile_rule(self):
        (self.directory / "src").mkdir()
        (self.directory / "src/image-image.c").write_text(
            "int image_value(void) { return 7; }\n")
        self.makefile(
            ".PHONY: all FORCE\n"
            "all: build/image-image.o\n"
            "build/%-image.o: build/%-image.c\n"
            "build/%.c: src/%.c FORCE\n"
            "\t@mkdir -p $(@D); cp $< $@\n"
            "build/%.o: build/%.c FORCE\n"
            "\t@printf 'compiled:%s' '$<' > $@\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "build/image-image.o").read_text(),
                         "compiled:build/image-image.c")
        (self.directory / "build/image-image.o").unlink()
        self.ninja()
        self.assertEqual((self.directory / "build/image-image.o").read_text(),
                         "compiled:build/image-image.c")

    def test_equal_stem_rules_keep_definition_order_across_trie_paths(self):
        (self.directory / "dep-a").touch()
        (self.directory / "dep-b").touch()
        self.makefile(
            "all: ab\n"
            "a%: dep-a\n"
            "\t@printf a-first > $@\n"
            "%b: dep-b\n"
            "\t@printf percent-b-second > $@\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "ab").read_text(), "a-first")
        (self.directory / "ab").unlink()
        self.ninja()
        self.assertEqual((self.directory / "ab").read_text(), "a-first")

    def test_replaced_implicit_rule_keeps_original_definition_position(self):
        (self.directory / "dep-a").touch()
        (self.directory / "dep-b").touch()
        self.makefile(
            "all: ab\n"
            "a%: dep-a\n"
            "\t@printf old-first > $@\n"
            "%b: dep-b\n"
            "\t@printf percent-b-second > $@\n"
            "a%: dep-a\n"
            "\t@printf replacement-first > $@\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "ab").read_text(),
                         "replacement-first")
        (self.directory / "ab").unlink()
        self.ninja()
        self.assertEqual((self.directory / "ab").read_text(),
                         "replacement-first")

    def test_pattern_and_explicit_appends_both_apply(self):
        self.makefile("FLAGS := base\n%.o: FLAGS += pattern\n"
                      "a.o: FLAGS += exact\nall: a.o\na.o:\n"
                      "\t@printf '%s' '$(FLAGS)' > result\n")
        self.direct("all")
        self.assertEqual((self.directory / "result").read_text(), "base pattern exact")
        (self.directory / "result").unlink()
        self.ninja()
        self.assertEqual((self.directory / "result").read_text(), "base pattern exact")

    def test_one_job_never_overlaps_recipes(self):
        self.makefile("all: a b\na b:\n\t@python3 worker.py $@\n")
        (self.directory / "worker.py").write_text(
            "from pathlib import Path\nimport json, sys, time\n"
            "start = time.monotonic()\n"
            "time.sleep(0.1)\n"
            "Path(sys.argv[1]).write_text(json.dumps([start, time.monotonic()]))\n",
            encoding="utf-8",
        )
        self.direct("-j1")
        first = json.loads((self.directory / "a").read_text())
        second = json.loads((self.directory / "b").read_text())
        self.assertGreaterEqual(max(first[0], second[0]),
                                min(first[1], second[1]))

    def test_dotted_source_to_dotless_destination_suffix(self):
        (self.directory / "zipfile.c").write_text("source")
        self.makefile(
            ".SUFFIXES:\n"
            ".SUFFIXES: _.o .o .c\n"
            ".c_.o:\n\t@cp $< $@\n"
            "all: zipfile_.o\n"
        )
        self.direct("all")
        self.assertEqual((self.directory / "zipfile_.o").read_text(), "source")


if __name__ == "__main__":
    unittest.main()
