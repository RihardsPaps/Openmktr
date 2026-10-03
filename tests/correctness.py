#!/usr/bin/env python3
"""Behavioral checks for build correctness across direct and Ninja modes."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


KATI = Path(os.environ.get(
    "KATI_BINARY", Path(__file__).resolve().parents[1] / "ckati"
)).resolve()


class BuildCorrectness(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kati-correctness-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def makefile(self, text):
        (self.directory / "Makefile").write_text(text, encoding="utf-8")

    def run_command(self, *args, success=True):
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
        self.assertRegex(output, r"\AGNU Make 4\.2\.1\nckati [^\n]+\n\Z")
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

    def test_failed_ninja_recipe_stays_failed(self):
        self.makefile("all:\n\t@sleep 0.05; echo partial > all; false\n")
        self.direct("--ninja")
        status, _ = self.run_command("sh", "./ninja.sh", "-j1", success=False)
        self.assertNotEqual(status, 0)

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
