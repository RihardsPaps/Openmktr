// Copyright 2015 Google Inc. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build ignore

#include "flags.h"
#include "fileutil.h"

#include <stdlib.h>
#include <unistd.h>

#include "log.h"
#include "strutil.h"

Flags g_flags;

// Return the variable name from a command-line assignment.  The suffix is
// kept here because assignment operators have different precedence rules:
// plain '=' and ':=' replace an inherited command-line value, while '+=' and
// '?=' intentionally operate conditionally/on top of it.
static std::string_view CommandLineVariableName(std::string_view assignment) {
  size_t equal = assignment.find('=');
  if (equal == std::string_view::npos || equal == 0)
    return {};

  std::string_view name = assignment.substr(0, equal);
  if (!name.empty() &&
      (name.back() == ':' || name.back() == '+' || name.back() == '?')) {
    name.remove_suffix(1);
  }
  return TrimSpace(name);
}

static bool ReplacesInheritedCommandLineValue(std::string_view assignment) {
  size_t equal = assignment.find('=');
  if (equal == std::string_view::npos || equal == 0)
    return false;
  std::string_view name = assignment.substr(0, equal);
  return name.back() != '+' && name.back() != '?';
}

static Flags::OutputSync ParseOutputSync(std::string_view value) {
  if (value.empty() || value == "target")
    return Flags::OutputSync::kTarget;
  if (value == "line")
    return Flags::OutputSync::kLine;
  if (value == "none")
    return Flags::OutputSync::kNone;
  if (value == "recurse")
    return Flags::OutputSync::kRecurse;
  ERROR("Invalid --output-sync mode: %.*s", static_cast<int>(value.size()),
        value.data());
  return Flags::OutputSync::kTarget;
}

static bool ParseCommandLineOptionWithArg(std::string_view option,
                                          char* argv[],
                                          int* index,
                                          const char** out_arg) {
  const char* arg = argv[*index];
  if (!HasPrefix(arg, option))
    return false;
  if (arg[option.size()] == '\0') {
    ++*index;
    *out_arg = argv[*index];
    return true;
  }
  if (arg[option.size()] == '=') {
    *out_arg = arg + option.size() + 1;
    return true;
  }
  // E.g, -j999
  if (option.size() == 2) {
    *out_arg = arg + option.size();
    return true;
  }
  return false;
}

void Flags::Parse(int argc, char** argv) {
  executable_path = GetExecutablePath();
  subkati_args.push_back(executable_path.c_str());
  num_jobs = num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  const char* num_jobs_str;
  const char* old_file;
  const char* what_if_file;
  const char* writable_str;
  const char* variable_assignment_trace_filter;

  if (const char* kati_jobs = getenv("KATI_JOBS")) {
    char* end = nullptr;
    long value = strtol(kati_jobs, &end, 10);
    if (end != kati_jobs && *end == '\0' && value > 0)
      num_jobs = value;
  }

  if (const char* makeflags = getenv("MAKEFLAGS")) {

    // GNU make uses backslash escaping in MAKEFLAGS/MAKEOVERRIDES so that
    // whitespace inside a command-line variable value survives recursive
    // make invocations.  WordScanner is intentionally a simple whitespace
    // scanner and is used throughout Kati, so do not change it globally.
    //
    // Parse MAKEFLAGS here with the small amount of escaping needed for
    // recursive command-line variable propagation.
    std::vector<std::string> makeflags_tokens;
    std::string token;
    bool escaped = false;

    for (const char* p = makeflags;; ++p) {
      char c = *p;

      if (escaped) {
        token += c;
        escaped = false;
        continue;
      }

      if (c == '\\') {
        escaped = true;
        continue;
      }

      if (c == '\0') {
        if (!token.empty())
          makeflags_tokens.push_back(std::move(token));
        break;
      }

      if (isspace(static_cast<unsigned char>(c))) {
        if (!token.empty()) {
          makeflags_tokens.push_back(std::move(token));
          token.clear();
        }
        continue;
      }

      token += c;
    }

    for (const std::string& tok : makeflags_tokens) {
      if (!HasPrefix(tok, "-") && tok.find('=') != std::string::npos) {
        const std::string_view name = CommandLineVariableName(tok);
        if (name == "MAKEFLAGS" || name == "MAKEOVERRIDES")
          continue;
        cl_vars.push_back(Intern(tok).str());
      }
    }
  }

  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    bool should_propagate = true;
    int pi = i;

    if (arg[0] == '-' && arg[1] != '-' &&
        arg[1] != '\0' && arg[2] != '\0') {
      bool valid_cluster = true;
      bool handled = false;

      for (size_t pos = 1; arg[pos] != '\0'; pos++) {
        char option = arg[pos];

        if (option == 'c' || option == 'd' || option == 'i' ||
            option == 'n' || option == 'q' || option == 's' ||
            option == 't' || option == 'k' || option == 'o') {
          handled = true;
          continue;
        }

        if (option == 'C' || option == 'f' ||
            option == 'j') {
          handled = true;

          const char* value = arg + pos + 1;
          if (*value == '\0') {
            if (i + 1 >= argc) {
              valid_cluster = false;
              break;
            }
            value = argv[++i];
          }

          if (option == 'C') {
            working_dir = value;
            should_propagate = false;
          } else if (option == 'f') {
            makefile = value;
            should_propagate = false;
          } else {
            num_jobs = strtol(value, NULL, 10);
            if (num_jobs <= 0)
              ERROR("Invalid -j flag: %s", value);
          }

          if (arg[pos + 1] != '\0')
            break;

          break;
        }

        valid_cluster = false;
        break;
      }

      if (valid_cluster && handled)
        continue;
    }

    if (!strcmp(arg, "-f")) {
      makefile = argv[++i];
      should_propagate = false;
    } else if (!strcmp(arg, "-c")) {
      is_syntax_check_only = true;
    } else if (!strcmp(arg, "-i")) {
      is_dry_run = true;
    } else if (!strcmp(arg, "-n") || !strcmp(arg, "--just-print") ||
               !strcmp(arg, "--dry-run")) {
      is_dry_run = true;
    } else if (!strcmp(arg, "-q") || !strcmp(arg, "--question")) {
      is_question = true;
    } else if (!strcmp(arg, "-k") || !strcmp(arg, "--keep-going")) {
      keep_going = true;
    } else if (ParseCommandLineOptionWithArg("-o", argv, &i, &old_file)) {
      old_files.emplace_back(TrimLeadingCurdir(old_file));
    } else if (ParseCommandLineOptionWithArg("--old-file", argv, &i,
                                             &old_file)) {
      old_files.emplace_back(TrimLeadingCurdir(old_file));
    } else if (ParseCommandLineOptionWithArg("-W", argv, &i,
                                             &what_if_file)) {
      what_if_files.emplace_back(TrimLeadingCurdir(what_if_file));
    } else if (ParseCommandLineOptionWithArg("--what-if", argv, &i,
                                             &what_if_file) ||
               ParseCommandLineOptionWithArg("--new-file", argv, &i,
                                             &what_if_file)) {
      what_if_files.emplace_back(TrimLeadingCurdir(what_if_file));
    } else if (!strcmp(arg, "-t") || !strcmp(arg, "--touch")) {
      is_touch = true;
    } else if (!strcmp(arg, "-s") || !strcmp(arg, "--silent")) {
      is_silent_mode = true;
    } else if (!strcmp(arg, "--output-sync")) {
      output_sync = OutputSync::kTarget;
    } else if (HasPrefix(arg, "--output-sync=")) {
      output_sync = ParseOutputSync(arg + strlen("--output-sync="));
    } else if (!strcmp(arg, "-d")) {
      enable_debug = true;
    } else if (!strcmp(arg, "--kati_stats")) {
      enable_stat_logs = true;
    } else if (!strcmp(arg, "--ninja_stats")) {
      ninja_stats = true;
      should_propagate = false;
    } else if (!strcmp(arg, "--warn")) {
      enable_kati_warnings = true;
    } else if (!strcmp(arg, "--ninja")) {
      generate_ninja = true;
    } else if (!strcmp(arg, "--no-print-directory")) {
      // GNU make exposes this command-line option through MAKEFLAGS.
      // Recursive makefiles use $(MAKEFLAGS) to decide whether another
      // bootstrap invocation is necessary.
      no_print_directory = true;
    } else if (!strcmp(arg, "--empty_ninja_file")) {
      generate_empty_ninja = true;
    } else if (!strcmp(arg, "--gen_all_targets")) {
      gen_all_targets = true;
    } else if (!strcmp(arg, "--regen")) {
      // TODO: Make this default.
      regen = true;
    } else if (!strcmp(arg, "--regen_debug")) {
      regen_debug = true;
    } else if (!strcmp(arg, "--regen_ignoring_kati_binary")) {
      regen_ignoring_kati_binary = true;
    } else if (!strcmp(arg, "--dump_kati_stamp")) {
      dump_kati_stamp = true;
      regen_debug = true;
    } else if (!strcmp(arg, "--detect_android_echo")) {
      detect_android_echo = true;
    } else if (!strcmp(arg, "--detect_depfiles")) {
      detect_depfiles = true;
    } else if (!strcmp(arg, "--color_warnings")) {
      color_warnings = true;
    } else if (!strcmp(arg, "--no_builtin_rules")) {
      no_builtin_rules = true;
    } else if (!strcmp(arg, "--no_ninja_prelude")) {
      no_ninja_prelude = true;
    } else if (!strcmp(arg, "--use_ninja_phony_output")) {
      use_ninja_phony_output = true;
    } else if (!strcmp(arg, "--use_ninja_validations")) {
      use_ninja_validations = true;
    } else if (!strcmp(arg, "--werror_find_emulator")) {
      werror_find_emulator = true;
    } else if (!strcmp(arg, "--werror_overriding_commands")) {
      werror_overriding_commands = true;
    } else if (!strcmp(arg, "--warn_implicit_rules")) {
      warn_implicit_rules = true;
    } else if (!strcmp(arg, "--warn-undefined-variables")) {
      warn_undefined_variables = true;
    } else if (!strcmp(arg, "--werror_implicit_rules")) {
      werror_implicit_rules = true;
    } else if (!strcmp(arg, "--warn_suffix_rules")) {
      warn_suffix_rules = true;
    } else if (!strcmp(arg, "--werror_suffix_rules")) {
      werror_suffix_rules = true;
    } else if (!strcmp(arg, "--top_level_phony")) {
      top_level_phony = true;
    } else if (!strcmp(arg, "--warn_real_to_phony")) {
      warn_real_to_phony = true;
    } else if (!strcmp(arg, "--werror_real_to_phony")) {
      warn_real_to_phony = true;
      werror_real_to_phony = true;
    } else if (!strcmp(arg, "--warn_phony_looks_real")) {
      warn_phony_looks_real = true;
    } else if (!strcmp(arg, "--werror_phony_looks_real")) {
      warn_phony_looks_real = true;
      werror_phony_looks_real = true;
    } else if (!strcmp(arg, "--werror_writable")) {
      werror_writable = true;
    } else if (!strcmp(arg, "--warn_real_no_cmds_or_deps")) {
      warn_real_no_cmds_or_deps = true;
    } else if (!strcmp(arg, "--werror_real_no_cmds_or_deps")) {
      warn_real_no_cmds_or_deps = true;
      werror_real_no_cmds_or_deps = true;
    } else if (!strcmp(arg, "--warn_real_no_cmds")) {
      warn_real_no_cmds = true;
    } else if (!strcmp(arg, "--werror_real_no_cmds")) {
      warn_real_no_cmds = true;
      werror_real_no_cmds = true;
    } else if (ParseCommandLineOptionWithArg("-C", argv, &i, &working_dir)) {
      should_propagate = false;
    } else if (ParseCommandLineOptionWithArg("--dump_include_graph", argv, &i,
                                             &dump_include_graph)) {
    } else if (ParseCommandLineOptionWithArg("--dump_variable_assignment_trace",
                                             argv, &i,
                                             &dump_variable_assignment_trace)) {
    } else if (ParseCommandLineOptionWithArg(
                   "--variable_assignment_trace_filter", argv, &i,
                   &variable_assignment_trace_filter)) {
      for (std::string_view pat :
           WordScanner(variable_assignment_trace_filter)) {
        traced_variables_pattern.push_back(Pattern(pat));
      }
    } else if (ParseCommandLineOptionWithArg("-j", argv, &i, &num_jobs_str)) {
      num_jobs = strtol(num_jobs_str, NULL, 10);
      if (num_jobs <= 0) {
        ERROR("Invalid -j flag: %s", num_jobs_str);
      }
    } else if (ParseCommandLineOptionWithArg("--remote_num_jobs", argv, &i,
                                             &num_jobs_str)) {
      remote_num_jobs = strtol(num_jobs_str, NULL, 10);
      if (remote_num_jobs <= 0) {
        ERROR("Invalid -j flag: %s", num_jobs_str);
      }
    } else if (ParseCommandLineOptionWithArg("--ninja_suffix", argv, &i,
                                             &ninja_suffix)) {
    } else if (ParseCommandLineOptionWithArg("--ninja_dir", argv, &i,
                                             &ninja_dir)) {
    } else if (!strcmp(arg, "--use_find_emulator")) {
      use_find_emulator = true;
    } else if (ParseCommandLineOptionWithArg(
                   "--ignore_optional_include", argv, &i,
                   &ignore_optional_include_pattern)) {
    } else if (ParseCommandLineOptionWithArg("--ignore_dirty", argv, &i,
                                             &ignore_dirty_pattern)) {
    } else if (ParseCommandLineOptionWithArg("--no_ignore_dirty", argv, &i,
                                             &no_ignore_dirty_pattern)) {
    } else if (ParseCommandLineOptionWithArg("--writable", argv, &i,
                                             &writable_str)) {
      writable.push_back(writable_str);
    } else if (ParseCommandLineOptionWithArg("--default_pool", argv, &i,
                                             &default_pool)) {
    } else if (!strcmp(arg, "--emit_sandbox_disabled")) {
      emit_sandbox_disabled = true;
    } else if (arg[0] == '-') {
      ERROR("Unknown flag: %s", arg);
    } else {
      if (strchr(arg, '=')) {
        // MAKEFLAGS and MAKEOVERRIDES are transport variables, not ordinary
        // recursive command-line overrides.  Treating a packed
        // MAKEOVERRIDES=... argument as a command-line variable preserves
        // stale state and causes the next Kati level to re-promote it over
        // the environment established by the invoking recipe.
        const std::string_view name = CommandLineVariableName(arg);
        if (name == "MAKEFLAGS" || name == "MAKEOVERRIDES") {
          should_propagate = false;
          continue;
        }

        // GNU make applies plain command-line assignments in order, so the
        // last assignment for a variable wins.  This applies both to values
        // inherited through MAKEFLAGS and to earlier assignments on argv.
        // Keep '+=' and '?=' entries so their distinct command-line semantics
        // remain intact.
        if (ReplacesInheritedCommandLineValue(arg)) {
          std::string_view name = CommandLineVariableName(arg);
          for (size_t j = 0; j < cl_vars.size();) {
            if (CommandLineVariableName(cl_vars[j]) == name) {
              cl_vars.erase(cl_vars.begin() + j);
            } else {
              ++j;
            }
          }
        }
        cl_vars.push_back(arg);
        should_propagate = false;
      } else {
        should_propagate = false;
        targets.push_back(Intern(TrimLeadingCurdir(arg)));
      }
    }

    if (should_propagate) {
      for (; pi <= i; pi++) {
        subkati_args.push_back(argv[pi]);
      }
    }
  }

  if (traced_variables_pattern.size() &&
      dump_variable_assignment_trace == nullptr) {
    ERROR(
        "--variable_assignment_trace_filter is valid only together with "
        "--dump_variable_assignment_trace");
  }
}
