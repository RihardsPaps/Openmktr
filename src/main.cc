// FORK MODIFICATION NOTICE (2026)
// Changed by the GNU-free Kati fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
// at the repository root. Original notices below remain applicable.

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

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string_view>
#include <unordered_set>

#include "eval.h"
#include "exec.h"
#include "file.h"
#include "file_cache.h"
#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "func.h"
#include "log.h"
#include "ninja.h"
#include "parser.h"
#include "regen.h"
#include "regen_dump.h"
#include "rule.h"
#include "stats.h"
#include "stmt.h"
#include "stringprintf.h"
#include "strutil.h"
#include "symtab.h"
#include "timeutil.h"
#include "var.h"

static char** g_argv;

// We know that there are leaks in Kati. Turn off LeakSanitizer by default.
extern "C" const char* __asan_default_options() {
  return "detect_leaks=0:allow_user_segv_handler=1";
}

static void ReadBootstrapMakefile(const std::vector<Symbol>& targets,
                                  std::vector<Stmt*>* stmts) {
  std::string bootstrap =
      ("CC?=cc\n"
#if defined(__APPLE__)
       "CXX?=c++\n"
#else
       "CXX?=c++\n"
#endif
       "AR?=ar\n"
       "ARFLAGS?=rv\n"
       "CPP?=$(CC) -E\n"
       "FC?=f77\n"
       "RANLIB?=ranlib\n"
       "RM?=rm -f\n"
       // These are GNU make's default compilation variables. Projects often
       // reuse them in explicit pattern rules rather than spelling out the
       // compiler command, even when they provide their own dependency flags.
       "COMPILE.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c\n"
       "COMPILE.f = $(FC) $(FFLAGS) $(TARGET_ARCH) -c\n"
       "COMPILE.S = $(CC) $(ASFLAGS) $(CPPFLAGS) $(TARGET_MACH) -c\n"
       "LINK.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)\n"
       "LINK.o = $(CC) $(LDFLAGS) $(TARGET_ARCH)\n"
       "OUTPUT_OPTION = -o $@\n"
       // Pretend to be GNU make 4.2.1, for compatibility.
       "MAKE_VERSION?=4.2.1\n"
       ".FEATURES?=output-sync undefine\n"
       "KATI?=ckati\n"
       // Overwrite $SHELL environment variable.
       "SHELL=/bin/sh\n"
       // TODO: Add more builtin vars.
      );

  if (!g_flags.no_builtin_rules) {
    bootstrap += (
        // http://www.gnu.org/software/make/manual/make.html#Catalogue-of-Rules
        // The document above is actually not correct. See default.c:
        // http://git.savannah.gnu.org/cgit/make.git/tree/default.c?id=4.1
        ".c.o:\n"
        "\t$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c -o $@ $<\n"
        ".cc.o:\n"
        "\t$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c -o $@ $<\n"
        ".f.o:\n"
        "\t$(COMPILE.f) $(OUTPUT_OPTION) $<\n"
        // TODO: Add more builtin rules.
    );
  }
  // MAKE is a public GNU make variable and is commonly expanded both as
  // ${MAKE} and as "${MAKE}". It must always be one executable pathname.
  // Recursive options belong in MAKEFLAGS; putting them in MAKE makes the
  // quoted form try to execute a pathname containing spaces, and makes
  // deferred Kbuild recursion lose its environment. This applies equally to
  // direct Kati execution and to generated Ninja recipes.
  const std::string& make_command = g_flags.executable_path;

  // MAKE is a public GNU make variable and is frequently expanded by a shell
  // script as either ${MAKE} or "${MAKE}".  It must therefore contain one
  // executable command, not an environment-assignment prefix: shell parameter
  // expansion does not re-parse assignment words, and a multi-word value also
  // fails when quoted.  Recursive Kati commands inherit KATI_JOBS and the
  // shared jobserver from the invoking wrapper at run time.
  //
  // In Ninja mode recursive Kati invocations are executed later, outside the
  // evaluator that is currently processing this makefile. Preserve the local
  // compiler flags on the recursive command, as recursive make does.
  // Keep MAKE exactly executable-shaped.  Appending MAKEOVERRIDES here not
  // only mixed command-line arguments into a public command variable, it also
  // left a trailing space when MAKEOVERRIDES was empty.  Both forms are
  // observable with shell scripts: ${MAKE} is reparsed as words, while
  // "${MAKE}" is one pathname and must name the executable exactly.
  bootstrap += StringPrintf("MAKE = %s\n", make_command.c_str());

  // GNU make propagates command-line variable assignments through
  // MAKEOVERRIDES and MAKEFLAGS when invoking a recursive make.
  //
  // The command-line assignments have not been evaluated yet at this point,
  // so the effective values cannot be constructed here.  They are installed
  // after bootstrap evaluation below.

  // GNU make normalizes leading "./" in command-line goals when exposing
  // them through MAKECMDGOALS.
  std::vector<Symbol> makecmdgoals;
  makecmdgoals.reserve(targets.size());
  for (Symbol target : targets) {
    makecmdgoals.push_back(Intern(TrimLeadingCurdir(target.str())));
  }

  bootstrap += StringPrintf("MAKECMDGOALS?=%s\n",
                            JoinSymbols(makecmdgoals, " ").c_str());
  bootstrap += StringPrintf("MAKELEVEL:=%d\n", g_flags.make_level);

  char cwd[PATH_MAX];
  if (!getcwd(cwd, PATH_MAX)) {
    fprintf(stderr, "getcwd failed\n");
    CHECK(false);
  }
  bootstrap += StringPrintf("CURDIR:=%s\n", cwd);
  Parse(Intern(bootstrap).str(), Loc("*bootstrap*", 0), stmts);
}

static void SetVar(std::string_view l,
                   VarOrigin origin,
                   Frame* definition,
                   Loc loc) {
  size_t found = l.find('=');
  CHECK(found != std::string::npos);
  Symbol lhs = Intern(l.substr(0, found));
  std::string_view rhs = Intern(l.substr(found + 1)).str();
  lhs.SetGlobalVar(
      new RecursiveVar(Value::NewLiteral(rhs), origin, definition, loc, rhs));
}

static std::string EscapeMakeOverrideValue(std::string_view value) {
  std::string result;
  result.reserve(value.size());

  for (char c : value) {
    if (c == '\\' || c == ' ' || c == '\t') {
      result += '\\';
    }
    result += c;
  }

  return result;
}

static std::string BuildMakeOverrides(Evaluator* ev) {
  std::vector<Symbol> names;
  std::unordered_set<Symbol> seen;

  for (std::string_view assignment : g_flags.cl_vars) {
    size_t equal = assignment.find('=');
    if (equal == std::string_view::npos || equal == 0)
      continue;

    std::string_view lhs = assignment.substr(0, equal);

    // Strip the assignment operator from :=, +=, and ?=.
    if (!lhs.empty() &&
        (lhs.back() == ':' || lhs.back() == '+' || lhs.back() == '?')) {
      lhs.remove_suffix(1);
    }

    lhs = TrimSpace(lhs);
    if (lhs.empty())
      continue;

    // These two variables are the transport used to propagate command-line
    // state.  They are not user overrides.  Treating them as overrides makes
    // a recursive invocation serialize its already-expanded MAKEFLAGS back
    // into MAKEFLAGS, so every recursion level accumulates compiler options
    // and eventually re-tokenizes values such as -m64 as --64.
    if (lhs == "MAKEFLAGS" || lhs == "MAKEOVERRIDES")
      continue;

    Symbol name = Intern(lhs);
    if (seen.insert(name).second)
      names.push_back(name);
  }

  std::string result;

  for (Symbol name : names) {
    // MAKEOVERRIDES carries the command-line override, not the variable's
    // value after the makefile has modified it.  For example, a makefile may
    // define FOO from another variable and then use "FOO += x". Serializing
    // ev->LookupVar(FOO)->Eval() would include x in the recursive command
    // line; the recursive makefile would append x again at every level.
    // Preserve the original assignment text so command-line-origin
    // semantics survive recursive Kati invocations.
    for (std::string_view assignment : g_flags.cl_vars) {
      size_t equal = assignment.find('=');
      if (equal == std::string_view::npos || equal == 0)
        continue;

      std::string_view lhs = assignment.substr(0, equal);
      if (!lhs.empty() &&
          (lhs.back() == ':' || lhs.back() == '+' || lhs.back() == '?')) {
        lhs.remove_suffix(1);
      }
      lhs = TrimSpace(lhs);
      if (lhs != name.str())
        continue;

      Var* var = ev->LookupVar(name);
      if (!var->IsDefined() || var->Origin() != VarOrigin::COMMAND_LINE)
        break;

      if (!result.empty())
        result += ' ';

      // GNU make transports the resulting unevaluated value as an ordinary
      // override. Replaying += in a child would append to the already
      // exported environment value (and may evaluate a deferred expression
      // before the child's makefile has defined its variables).
      const bool simple = std::string_view(var->Flavor()) == "simple";
      result += name.str();
      result += simple ? ":=" : "=";
      std::string value(var->String());
      if (simple) {
        // A simply expanded override must remain literal when a child
        // parses it from MAKEFLAGS.
        for (size_t pos = 0; (pos = value.find('$', pos)) != std::string::npos;
             pos += 2)
          value.insert(pos, 1, '$');
      }
      result += EscapeMakeOverrideValue(value);
      break;
    }
  }

  return result;
}

static void UpdateMakeFlags(Evaluator* ev) {
  // MAKEOVERRIDES is special in GNU make: a makefile may clear it to stop
  // command-line assignments from propagating to recursive makes.  The
  // automatically generated value must therefore have default origin, so a
  // later makefile assignment can replace it.
  Var* existing_overrides = ev->LookupVar(Intern("MAKEOVERRIDES"));
  const bool explicit_overrides =
      existing_overrides->IsDefined() &&
      (existing_overrides->Origin() == VarOrigin::FILE ||
       existing_overrides->Origin() == VarOrigin::OVERRIDE ||
       existing_overrides->Origin() == VarOrigin::COMMAND_LINE);
  const std::string overrides = explicit_overrides
                                    ? existing_overrides->Eval(ev)
                                    : BuildMakeOverrides(ev);

  if (!overrides.empty()) {
    SetVar("MAKEOVERRIDES=" + overrides, VarOrigin::DEFAULT, nullptr,
           Loc("*bootstrap*", 0));
    setenv("MAKEOVERRIDES", overrides.c_str(), 1);
  } else {
    SetVar("MAKEOVERRIDES=", VarOrigin::DEFAULT, nullptr,
           Loc("*bootstrap*", 0));
    setenv("MAKEOVERRIDES", "", 1);
  }

  Var* makeflags = ev->LookupVar(Intern("MAKEFLAGS"));
  std::string makeflags_value;
  if (makeflags->IsDefined())
    makeflags_value = makeflags->Eval(ev);

  // GNU make uses "--" as the separator between options and command-line
  // variable overrides.  A makefile may append options after an inherited
  // separator (Kbuild appends -rR this way), but recursive invocations must
  // see those options before the separator.
  size_t separator = std::string::npos;
  for (size_t candidate = makeflags_value.find("--");
       candidate != std::string::npos;
       candidate = makeflags_value.find("--", candidate + 2)) {
    const bool at_start =
        candidate == 0 ||
        isspace(static_cast<unsigned char>(makeflags_value[candidate - 1]));
    const size_t end = candidate + 2;
    const bool at_end =
        end == makeflags_value.size() ||
        isspace(static_cast<unsigned char>(makeflags_value[end]));
    if (at_start && at_end) {
      separator = candidate;
      break;
    }
  }
  if (separator != std::string::npos) {
    const std::string inherited_tail = makeflags_value.substr(separator + 2);
    makeflags_value.erase(separator);

    // MAKEFLAGS/MAKEOVERRIDES escape whitespace inside assignments.  Use the
    // same escape-aware tokenization as Flags::Parse; WordScanner would split
    // CFLAGS=-O2\ -m64 into a bogus top-level -m64 option on the second
    // normalization pass.
    std::string token;
    bool escaped = false;
    auto keep_option = [&makeflags_value](const std::string& value) {
      if (!value.empty() && value[0] == '-') {
        if (!makeflags_value.empty())
          makeflags_value += ' ';
        makeflags_value += value;
      }
    };
    for (char c : inherited_tail) {
      if (escaped) {
        token += c;
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (isspace(static_cast<unsigned char>(c))) {
        if (!token.empty()) {
          keep_option(token);
          token.clear();
        }
      } else {
        token += c;
      }
    }
    if (escaped)
      token += '\\';
    if (!token.empty())
      keep_option(token);
  }

  while (!makeflags_value.empty() &&
         (makeflags_value.back() == ' ' || makeflags_value.back() == '\n')) {
    makeflags_value.pop_back();
  }

  // Recursive make invocations inherit execution modes through MAKEFLAGS.
  // Keep these options in the normalized option section so a child Kati
  // process observes the same dry-run, question, keep-going, touch, and
  // silent behavior as GNU make.  Command-line assignments remain after the
  // separator below and are handled independently.
  auto append_option = [&makeflags_value](const char* option) {
    for (std::string_view token : WordScanner(makeflags_value)) {
      if (token == option)
        return;
    }
    if (!makeflags_value.empty())
      makeflags_value += ' ';
    makeflags_value += option;
  };
  if (g_flags.is_dry_run)
    append_option("-n");
  if (g_flags.is_question)
    append_option("-q");
  if (g_flags.keep_going)
    append_option("-k");
  if (g_flags.is_touch)
    append_option("-t");
  if (g_flags.is_silent_mode)
    append_option("-s");
  if (g_flags.no_builtin_rules)
    append_option("-r");
  if (g_flags.environment_overrides)
    append_option("-e");

  // Wrappers such as QEMU's Makefile forward this option to Ninja. Replace
  // inherited job limits with the effective limit selected for this process.
  std::string without_jobs;
  bool skip_job_count = false;
  for (std::string_view token : WordScanner(makeflags_value)) {
    if (skip_job_count) {
      skip_job_count = false;
      if (!token.empty() && isdigit(static_cast<unsigned char>(token[0])))
        continue;
    }
    if (token == "-j" || token == "--jobs") {
      skip_job_count = true;
      continue;
    }
    if (HasPrefix(token, "-j") || HasPrefix(token, "--jobs="))
      continue;
    if (!without_jobs.empty())
      without_jobs += ' ';
    without_jobs += token;
  }
  makeflags_value = std::move(without_jobs);
  const std::string jobs_option = "-j" + std::to_string(g_flags.num_jobs);
  append_option(jobs_option.c_str());

  // Options inherited from MAKEFLAGS are already present in makeflags_value.
  // Add only directories supplied on this command line, preserving their
  // order and escaping whitespace for the next invocation's option parser.
  for (const std::string& dir : g_flags.command_line_include_dirs) {
    if (!makeflags_value.empty())
      makeflags_value += ' ';
    makeflags_value += "-I";
    makeflags_value += EscapeMakeOverrideValue(dir);
  }

  if (g_flags.no_print_directory &&
      makeflags_value.find("--no-print-directory") == std::string::npos) {
    if (!makeflags_value.empty())
      makeflags_value += ' ';
    makeflags_value += "--no-print-directory";
  }

  if (!overrides.empty()) {
    if (!makeflags_value.empty())
      makeflags_value += ' ';
    makeflags_value += "-- ";
    makeflags_value += overrides;
  }

  // Older Automake recipes inspect MFLAGS when MAKE_VERSION is defined.
  // GNU make exports the options there, without command-line assignments.
  const size_t overrides_separator = makeflags_value.find(" -- ");
  const std::string mflags_value =
      overrides_separator == std::string::npos
          ? makeflags_value
          : makeflags_value.substr(0, overrides_separator);
  setenv("MFLAGS", mflags_value.c_str(), 1);
  SetVar("MAKEFLAGS=" + makeflags_value, VarOrigin::COMMAND_LINE, nullptr,
         Loc("*bootstrap*", 0));
  setenv("MAKEFLAGS", makeflags_value.c_str(), 1);
}

extern "C" char** environ;

class SegfaultHandler {
 public:
  explicit SegfaultHandler(Evaluator* ev);
  ~SegfaultHandler();

  void handle(int, siginfo_t*, void*);

 private:
  static SegfaultHandler* global_handler;

  void dumpstr(const char* s) const {
    (void)write(STDERR_FILENO, s, strlen(s));
  }
  void dumpint(int i) const {
    char buf[11];
    char* ptr = buf + sizeof(buf) - 1;

    if (i < 0) {
      i = -i;
      dumpstr("-");
    } else if (i == 0) {
      dumpstr("0");
      return;
    }

    *ptr = '\0';
    while (ptr > buf && i > 0) {
      *--ptr = '0' + (i % 10);
      i = i / 10;
    }

    dumpstr(ptr);
  }

  Evaluator* ev_;

  struct sigaction orig_action_;
  struct sigaction new_action_;
};

SegfaultHandler* SegfaultHandler::global_handler = nullptr;

SegfaultHandler::SegfaultHandler(Evaluator* ev) : ev_(ev) {
  CHECK(global_handler == nullptr);
  global_handler = this;

  // Construct an alternate stack, so that we can handle stack overflows.
  stack_t ss;
  ss.ss_sp = malloc(SIGSTKSZ * 2);
  CHECK(ss.ss_sp != nullptr);
  ss.ss_size = SIGSTKSZ * 2;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, nullptr) == -1) {
    PERROR("sigaltstack");
  }

  // Register our segfault handler using the alternate stack, falling
  // back to the default handler.
  sigemptyset(&new_action_.sa_mask);
  new_action_.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_RESETHAND;
  new_action_.sa_sigaction = [](int sig, siginfo_t* info, void* context) {
    if (global_handler != nullptr) {
      global_handler->handle(sig, info, context);
    }

    raise(SIGSEGV);
  };
  sigaction(SIGSEGV, &new_action_, &orig_action_);
}

void SegfaultHandler::handle(int sig, siginfo_t* info, void* context) {
  // Avoid fprintf in case it allocates or tries to do anything else that may
  // hang.
  dumpstr("*kati*: Segmentation fault, last evaluated line was ");
  dumpstr(ev_->loc().filename);
  dumpstr(":");
  dumpint(ev_->loc().lineno);
  dumpstr("\n");

  // Run the original handler, in case we've been preloaded with libSegFault
  // or similar.
  if (orig_action_.sa_sigaction != nullptr) {
    orig_action_.sa_sigaction(sig, info, context);
  }
}

SegfaultHandler::~SegfaultHandler() {
  sigaction(SIGSEGV, &orig_action_, nullptr);
  global_handler = nullptr;
}

static int Run(const std::vector<Symbol>& targets,
               const std::vector<std::string_view>& cl_vars,
               const std::string& orig_args,
               const std::string& invocation_dir) {
  double start_time = GetTime();

  const bool has_stdin_makefile =
      std::any_of(g_flags.makefiles.begin(), g_flags.makefiles.end(),
                  [](const char* name) { return strcmp(name, "-") == 0; });
  // Stdin has no persistent timestamp. Even a real file named '-' cannot
  // establish that the stream supplied to this invocation is unchanged.
  if (!has_stdin_makefile && g_flags.generate_ninja &&
      (g_flags.regen || g_flags.dump_kati_stamp)) {
    ScopedTimeReporter tr("regen check time");
    if (!NeedsRegen(start_time, orig_args)) {
      fprintf(stderr, "No need to regenerate ninja file\n");
      return 0;
    }
    if (g_flags.dump_kati_stamp) {
      printf("Need to regenerate ninja file\n");
      return 0;
    }
    ClearGlobCache();
  }

  std::vector<Stmt*> bootstrap_asts;
  ReadBootstrapMakefile(targets, &bootstrap_asts);

  for (;;) {
    Evaluator ev;
    if (!ev.Start()) {
      for (Stmt* stmt : bootstrap_asts)
        delete stmt;
      return 1;
    }

    Intern("MAKEFILE_LIST")
        .SetGlobalVar(new SimpleVar(StringPrintf(" %s", g_flags.makefiles[0]),
                                    VarOrigin::FILE, ev.CurrentFrame(),
                                    ev.loc()));

    for (char** p = environ; *p; p++) {
      const std::string_view entry(*p);
      const size_t equal_pos = entry.find('=');
      const std::string_view name = entry.substr(0, equal_pos);
      const bool internal_make_variable =
          name == "MAKEFLAGS" || name == "MAKEOVERRIDES" ||
          name == "MAKELEVEL" || name == "MFLAGS" || name == "SHELL" ||
          name == "CURDIR" || name == "MAKECMDGOALS" || name == "MAKEFILE_LIST";
      SetVar(*p,
             g_flags.environment_overrides && !internal_make_variable
                 ? VarOrigin::ENVIRONMENT_OVERRIDE
                 : VarOrigin::ENVIRONMENT,
             nullptr, Loc());
      const char* equal = strchr(*p, '=');
      if (equal != nullptr && strncmp(*p, "VPATH=", 6) == 0)
        ev.SetVpath("%", equal + 1);
    }

    SegfaultHandler segfault(&ev);

    {
      ScopedFrame frame(ev.Enter(FrameType::PHASE, "*bootstrap*", Loc()));
      ev.in_bootstrap();

      for (Stmt* stmt : bootstrap_asts) {
        LOG("%s", stmt->DebugString().c_str());
        stmt->Eval(&ev);
      }
    }

    {
      ScopedFrame frame(ev.Enter(FrameType::PHASE, "*command line*", Loc()));
      ev.in_command_line();

      for (std::string_view l : cl_vars) {
        std::vector<Stmt*> asts;
        // Hashes and preceding backslashes in argv assignments are literal.
        ParseArgvAssignment(l, Loc("*bootstrap*", 0), &asts);
        CHECK(asts.size() == 1);
        asts[0]->Eval(&ev);
      }
    }

    // At this point command-line assignments have been evaluated, so
    // reconstruct the propagation variables using their effective values.
    //
    // GNU make exposes command-line options such as --no-print-directory
    // through MAKEFLAGS, and command-line variable overrides after "--".
    // Keep the option section separate from the override section so recursive
    // invocations do not accumulate duplicate assignments.
    UpdateMakeFlags(&ev);

    ev.in_toplevel_makefile();

    {
      ScopedFrame eval_frame(ev.Enter(FrameType::PHASE, "*parse*", Loc()));
      ScopedTimeReporter tr("eval time");

      for (size_t i = 0; i < g_flags.makefiles.size(); ++i) {
        const char* makefile = g_flags.makefiles[i];
        if (i != 0) {
          Var* makefile_list = ev.LookupVar(Intern("MAKEFILE_LIST"));
          makefile_list->AppendVar(&ev, Value::NewLiteral(makefile));
        }

        ScopedFrame file_frame(ev.Enter(FrameType::PARSE, makefile, Loc()));
        const Makefile& mk =
            strcmp(makefile, "-") == 0
                ? MakefileCacheManager::Get().ReadStdinMakefile()
                : MakefileCacheManager::Get().ReadMakefile(makefile);
        for (Stmt* stmt : mk.stmts()) {
          LOG("%s", stmt->DebugString().c_str());
          stmt->Eval(&ev);
        }
      }
    }

    // Makefiles may modify MAKEFLAGS while they are parsed. Rebuild the
    // recursive propagation value after parsing so deferred Ninja recipes and
    // normal Kati recipes receive the normalized final option/override split.
    UpdateMakeFlags(&ev);

    for (ParseErrorStmt* err : GetParseErrors()) {
      WARN_LOC(err->loc(), "warning for parse error in an unevaluated line: %s",
               err->msg.c_str());
    }

    /*
     * GNU make compatibility:
     *
     * Makefiles named directly or reached by include are remade before
     * ordinary goals. If one changes, restart the entire evaluation.
     */
    if (!g_flags.makefiles.empty() || !ev.missing_includes().empty() ||
        !ev.included_makefiles().empty()) {
      std::vector<Symbol> include_targets;

      auto is_phony_makefile = [&](std::string_view filename) {
        filename = TrimLeadingCurdir(filename);
        for (const Rule* rule : ev.rules()) {
          if (std::find(rule->outputs.begin(), rule->outputs.end(),
                        Intern(".PHONY")) != rule->outputs.end()) {
            for (Symbol input : rule->inputs)
              if (TrimLeadingCurdir(input.str()) == filename)
                return true;
          }
        }
        return false;
      };
      auto has_unconditional_double_colon = [&](std::string_view filename) {
        filename = TrimLeadingCurdir(filename);
        for (const Rule* rule : ev.rules()) {
          if (!rule->is_double_colon || rule->cmds.empty() ||
              !rule->inputs.empty() || !rule->order_only_inputs.empty())
            continue;
          for (Symbol output : rule->outputs)
            if (TrimLeadingCurdir(output.str()) == filename)
              return true;
        }
        return false;
      };

      for (const char* makefile : g_flags.makefiles) {
        if (std::string_view(makefile) == "-" ||
            has_unconditional_double_colon(makefile))
          continue;
        // Phony makefiles are rebuilt once, but must not trigger a restart.
        // An arbitrary .DEFAULT or catch-all pattern must not try to remake
        // the parser input. Explicit Makefile rules are the common Autotools
        // case and are safe to execute before ordinary goals.
        const std::string_view filename = TrimLeadingCurdir(makefile);
        bool has_explicit_rule = false;
        for (const Rule* rule : ev.rules()) {
          for (Symbol output : rule->outputs) {
            if (TrimLeadingCurdir(output.str()) == filename &&
                !(rule->is_double_colon && rule->inputs.empty() &&
                  rule->order_only_inputs.empty()) &&
                (!rule->cmds.empty() || !rule->inputs.empty() ||
                 !rule->order_only_inputs.empty())) {
              has_explicit_rule = true;
              break;
            }
          }
          if (has_explicit_rule)
            break;
        }
        if (has_explicit_rule)
          include_targets.push_back(Intern(makefile));
      }

      for (const auto& include : ev.missing_includes()) {
        if (has_unconditional_double_colon(include.filename))
          continue;
        include_targets.push_back(Intern(include.filename));
      }
      for (const auto& include : ev.included_makefiles()) {
        if (has_unconditional_double_colon(include))
          continue;
        Symbol target = Intern(include);
        if (std::find(include_targets.begin(), include_targets.end(), target) ==
            include_targets.end()) {
          include_targets.push_back(target);
        }
      }

      if (include_targets.empty())
        goto includes_done;

      std::vector<NamedDepNode> include_nodes;

      {
        ScopedFrame frame(
            ev.Enter(FrameType::PHASE, "*remake included makefiles*", Loc()));

        ScopedTimeReporter tr("remake included makefiles time");

        // Included makefiles may create their own parent directories. Adding
        // synthetic directory edges here can introduce a cycle through a
        // project's prepare target before the include has been generated.
        ev.RefreshVpath();
        MakeDep(&ev, ev.rules(), ev.rule_vars(), include_targets,
                &include_nodes, false);
      }

      // Optional includes without a producing rule are valid and must remain
      // silent. Do not use has_rule alone as proof that an include was
      // remade: a dependency cycle may leave a rule-backed target untouched.
      // Restarting in that case re-enters the same missing-include path
      // forever (U-Boot's generated config headers expose this readily).
      std::vector<NamedDepNode> include_remake_nodes;
      std::vector<NamedDepNode> deferred_include_nodes;
      for (const auto& include : include_nodes) {
        if (!include.second->has_rule)
          continue;
        const DepNode* makefile_node = include.second;
        bool unconditional = false;
        for (size_t group = 0;
             group < makefile_node->double_colon_group_has_prerequisites.size();
             ++group) {
          if (!makefile_node->double_colon_group_has_prerequisites[group] &&
              std::find(makefile_node->double_colon_group_for_cmd.begin(),
                        makefile_node->double_colon_group_for_cmd.end(),
                        group) !=
                  makefile_node->double_colon_group_for_cmd.end())
            unconditional = true;
        }
        // Secondary expansion can turn a syntactically nonempty list into
        // an unconditional double-colon recipe. Do not remake parser inputs
        // with such recipes, just as for a literal empty prerequisite list.
        if (unconditional)
          continue;
        // A generated include can introduce rules needed to remake another
        // include. Build resolvable graphs first and restart before treating
        // those temporarily missing prerequisites as fatal (glibc does this
        // with soversions.mk and libc-modules.stmp).
        std::vector<const DepNode*> pending{include.second};
        std::unordered_set<const DepNode*> visited;
        bool ready = true;
        while (!pending.empty()) {
          const DepNode* node = pending.back();
          pending.pop_back();
          if (!visited.insert(node).second)
            continue;
          if (!node->has_rule && !node->is_phony &&
              !Exists(node->output.c_str())) {
            ready = false;
            break;
          }
          for (const auto& dep : node->deps)
            pending.push_back(dep.second);
          for (const auto& dep : node->order_onlys)
            pending.push_back(dep.second);
          for (const auto& dep : node->validations)
            pending.push_back(dep.second);
        }
        (ready ? include_remake_nodes : deferred_include_nodes)
            .push_back(include);
      }

      // If no include can make progress, preserve the normal missing-rule
      // diagnosis instead of silently ignoring a broken remake recipe.
      if (include_remake_nodes.empty())
        include_remake_nodes.swap(deferred_include_nodes);

      if (!include_remake_nodes.empty()) {
        std::vector<double> include_timestamps;
        for (const auto& include : include_remake_nodes)
          include_timestamps.push_back(GetTimestamp(include.first.str()));
        ExecResult include_result{false, false, false};
        {
          ScopedFrame frame(ev.Enter(FrameType::PHASE,
                                     "*execute included makefiles*", Loc()));

          ScopedTimeReporter tr("execute included makefiles time");

          // Remaking dependency includes can compile the entire project.
          // Use the same bounded scheduler as ordinary targets; evaluation
          // is serialized by the executor and graph edges enforce ordering.
          // GNU make runs included-makefile remaking normally under -t.
          // Touching Makefile itself on every evaluation would otherwise
          // restart this process forever, even when it is up to date.
          const bool touch_mode = g_flags.is_touch;
          g_flags.is_touch = false;
          include_result = Exec(include_remake_nodes, &ev);
          g_flags.is_touch = touch_mode;
          if (include_result.failed && !g_flags.keep_going) {
            ev.Finish();
            return 1;
          }
        }

        // GNU make restarts after an included makefile was actually rebuilt.
        // A prerequisite may run without rebuilding the included makefile.
        // Directory prerequisites in particular may run on every pass, so
        // restarting for any recipe in the include graph would loop forever.
        bool includes_changed = false;
        for (size_t i = 0; i < include_remake_nodes.size(); ++i) {
          if (is_phony_makefile(include_remake_nodes[i].first.str()))
            continue;
          if (GetTimestamp(include_remake_nodes[i].first.str()) !=
              include_timestamps[i])
            includes_changed = true;
        }
        if (!includes_changed) {
          if (!deferred_include_nodes.empty()) {
            const bool touch_mode = g_flags.is_touch;
            g_flags.is_touch = false;
            const ExecResult deferred_result =
                Exec(deferred_include_nodes, &ev);
            g_flags.is_touch = touch_mode;
            if (deferred_result.failed && !g_flags.keep_going) {
              ev.Finish();
              return 1;
            }
          }
          ev.Finish();
          goto includes_done;
        }

        ev.Finish();

        /*
         * The include may have been invisible to Glob() during the first
         * evaluation. Its cached result must not survive the restart.
         */
        ClearGlobCache();

        /*
         * Start over in a fresh Kati process.  A process restart resets the
         * makefile cache, glob cache, rule state, and evaluator together,
         * which is the same semantic boundary GNU make uses after remaking
         * an included makefile.
         */
        // A relative -C has already changed this process's working directory.
        // execvp preserves that directory, so replaying the original argv
        // would apply -C relative to itself (for example, subdir/subdir).
        // Restart from the directory in which this Kati invocation began.
        if (chdir(invocation_dir.c_str()) != 0) {
          ERROR(
              "*** failed to restore working directory before restarting Kati: "
              "%s",
              strerror(errno));
        }
        // A pipe cannot be reread after exec. Preserve a stdin Makefile in an
        // anonymous temporary file so the restarted evaluator sees the same
        // input after rebuilding an include.
        for (const char* makefile : g_flags.makefiles) {
          if (strcmp(makefile, "-") != 0)
            continue;
          const std::string& contents =
              MakefileCacheManager::Get().ReadStdinMakefile().buf();
          FILE* input = tmpfile();
          if (input == nullptr)
            PERROR("tmpfile for stdin Makefile failed");
          if (fwrite(contents.data(), 1, contents.size(), input) !=
                  contents.size() ||
              fflush(input) != 0 || fseek(input, 0, SEEK_SET) != 0 ||
              dup2(fileno(input), STDIN_FILENO) < 0)
            PERROR("preserving stdin Makefile failed");
          if (fclose(input) != 0)
            PERROR("close stdin Makefile copy failed");
          break;
        }
        execvp(g_argv[0], g_argv);
        ERROR(
            "*** failed to restart Kati after remaking included makefiles: %s",
            strerror(errno));
        return 1;
      }
    }

  includes_done:

    if (g_flags.dump_include_graph != nullptr)
      ev.DumpIncludeJSON(std::string(g_flags.dump_include_graph));

    std::vector<NamedDepNode> nodes;

    {
      ScopedFrame frame(
          ev.Enter(FrameType::PHASE, "*dependency analysis*", Loc()));

      ScopedTimeReporter tr("make dep time");

      ev.RefreshVpath();
      MakeDep(&ev, ev.rules(), ev.rule_vars(), targets, &nodes);
    }

    if (g_flags.is_syntax_check_only) {
      ev.Finish();

      for (Stmt* stmt : bootstrap_asts)
        delete stmt;

      return 0;
    }

    if (g_flags.generate_ninja) {
      ScopedFrame frame(
          ev.Enter(FrameType::PHASE, "*ninja generation*", Loc()));

      ScopedTimeReporter tr("generate ninja time");

      // Recursive build recipes commonly establish this shell variable from
      // the current output directory before expanding nested $(shell) calls.
      // During graph generation those calls are evaluated by Kati instead of
      // by the eventual recipe shell, so provide the same default when the
      // caller did not already set it.
      if (getenv("r") == nullptr) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != nullptr)
          setenv("r", cwd, 1);
      }

      GenerateNinja(nodes, &ev, orig_args, start_time);

      ev.DumpStackStats();
      ev.Finish();

      for (Stmt* stmt : bootstrap_asts)
        delete stmt;

      return 0;
    }

    for (const auto& p : ev.exports()) {
      const Symbol name = p.first;

      if (p.second) {
        // LookupVar can still expose the environment-origin object that was
        // present before a makefile assignment replaced the effective value.
        // EvalVar resolves the current scope, matching what recipes and the
        // Ninja environment snapshot observe.
        const std::string value = ev.EvalVar(name);
        setenv(name.c_str(), value.c_str(), 1);
      } else {
        LOG("unsetenv(%s)", name.c_str());
        unsetenv(name.c_str());
      }
    }

    {
      ScopedFrame frame(ev.Enter(FrameType::PHASE, "*execution*", Loc()));
      ScopedTimeReporter tr("exec time");

      const ExecResult execution = Exec(nodes, &ev);

      if (g_flags.is_question) {
        ev.DumpStackStats();
        ev.Finish();

        for (Stmt* stmt : bootstrap_asts)
          delete stmt;

        return execution.failed ? 2 : execution.needs_build ? 1 : 0;
      }

      if (execution.failed) {
        ev.DumpStackStats();
        ev.Finish();

        for (Stmt* stmt : bootstrap_asts)
          delete stmt;

        return 1;
      }
    }

    ev.DumpStackStats();
    ev.Finish();

    for (Stmt* stmt : bootstrap_asts)
      delete stmt;

    return 0;
  }
}

static void FindFirstMakefie() {
  if (!g_flags.makefiles.empty())
    return;
  if (Exists("GNUmakefile")) {
    g_flags.makefile = "GNUmakefile";
#if !defined(__APPLE__)
  } else if (Exists("makefile")) {
    g_flags.makefile = "makefile";
#endif
  } else if (Exists("Makefile")) {
    g_flags.makefile = "Makefile";
  }
  if (g_flags.makefile != nullptr)
    g_flags.makefiles.push_back(g_flags.makefile);
}

static void HandleRealpath(int argc, char** argv) {
  char buf[PATH_MAX];
  for (int i = 0; i < argc; i++) {
    if (realpath(argv[i], buf))
      printf("%s\n", buf);
  }
}

static int HandleFileRead(int argc, char** argv) {
  if (argc != 1)
    return 1;

  int fd = open(argv[0], O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT)
      return 0;
    return 1;
  }

  std::string out;
  char buf[8192];

  while (true) {
    ssize_t n = HANDLE_EINTR(read(fd, buf, sizeof(buf)));
    if (n < 0) {
      close(fd);
      return 1;
    }
    if (n == 0)
      break;
    out.append(buf, n);
  }

  if (close(fd) != 0)
    return 1;

  // GNU make's $(file <...) removes one trailing newline.
  if (!out.empty() && out.back() == '\n')
    out.pop_back();

  if (!out.empty() && fwrite(out.data(), 1, out.size(), stdout) != out.size())
    return 1;

  return 0;
}

int main(int argc, char* argv[]) {
  g_argv = argv;
  // Recursive invocations isolate ckati from a target project's runtime
  // libraries while it is being loaded. Restore that runtime environment
  // after the loader has started this process; recipes executed by this child
  // still need the target library path.
  if (const char* saved_ld_library_path =
          getenv("KATI_SAVED_LD_LIBRARY_PATH")) {
    setenv("LD_LIBRARY_PATH", saved_ld_library_path, 1);
    unsetenv("KATI_SAVED_LD_LIBRARY_PATH");
  }

  if (argc >= 2) {
    if (!strcmp(argv[1], "--realpath")) {
      HandleRealpath(argc - 2, argv + 2);
      return 0;
    } else if (!strcmp(argv[1], "--file-read")) {
      return HandleFileRead(argc - 2, argv + 2);
    } else if (!strcmp(argv[1], "--dump_stamp_tool")) {
      // Unfortunately, this can easily be confused with --dump_kati_stamp,
      // which prints debug info about the stamp while executing a normal kati
      // run. This tool flag only dumps information, and doesn't run the rest of
      // kati.
      return stamp_dump_main(argc, argv);
    }
  }
  std::string orig_args;
  for (int i = 0; i < argc; i++) {
    if (i)
      orig_args += ' ';
    orig_args += argv[i];
  }
  char invocation_dir_buf[PATH_MAX];
  if (!getcwd(invocation_dir_buf, sizeof(invocation_dir_buf)))
    PERROR("getcwd");
  const std::string invocation_dir(invocation_dir_buf);
  g_flags.Parse(argc, argv);
  // Direct recursive invocations must inherit the effective local job limit,
  // including a command-line -j override. Ninja's launcher supplies its own
  // shared jobserver; KATI_JOBS also bounds children launched by configure.
  const std::string job_limit = std::to_string(g_flags.num_jobs);
  setenv("KATI_JOBS", job_limit.c_str(), 1);
  if (g_flags.working_dir) {
    int ret = chdir(g_flags.working_dir);
    if (ret != 0)
      ERROR("*** %s: %s", g_flags.working_dir, strerror(errno));
  }
  FindFirstMakefie();
  if (g_flags.makefile == NULL)
    ERROR("*** No targets specified and no makefile found.");

  // This depends on command line flags.
  if (g_flags.use_find_emulator)
    InitFindEmulator();
  int r = Run(g_flags.targets, g_flags.cl_vars, orig_args, invocation_dir);
  ReportAllStats();
  return r;
}
