// FORK MODIFICATION NOTICE (2026)
// Read optional double-colon metadata through the shared dependency view.
// Launch checked generation plans without repeated shell/helper startup.
// Own generated launcher FIFOs and leases in the native supervisor.
// Own invocation state and publish coherent generations with typed freshness.
// Report supervisor phases and exclude diagnostic paths from graph identity.
// Suppress no-op announcements for internal included-makefile updates.
// Prepare immutable runtime contexts once per known Ninja invocation.
// Prune missing intermediates only for current consumers and known Ninja goals.
// Normalize MFLAGS definitions across generated-include restarts.
// Publish observed recipe inputs only after successful supervised execution.
// Scope dependency graphs and load checked large recipe scripts natively.
// Dispatch private runtime evaluation of serialized Ninja recipes.
// Compare generated freshness guards with native nanosecond timestamps.
// Let managed Ninja builds inherit a valid FIFO unless -j was explicit.
// Add configurable Ninja graph optimizations and content-preserving output
// generation. Forward load limits to Ninja and normalize recursive Make load
// options. Keep private graph controls out of Make variables and recipe
// exports. Supervise per-launcher load state and normalize options before
// Ninja's --. Add a locked, regenerating single-command Ninja build supervisor.
// Expose native output stamps for generated recipes' interruption cleanup.
// Pass Ninja output cleanup to the existing native recipe supervisor.
// Add workspace profile enumeration for the Portage manager.
// Dispatch optional persistent-workspace management to the Python frontend.
// Let changed workspace recipes bypass initial-file timestamp guards.
// Changed by the Openmktr fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Validate recursive reuse, regeneration inputs, and shared recipe budgets.
// Explain graph inputs and scheduler decisions without running recipes.
// Added an internal timestamp query used by generated Ninja recipes.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include "command.h"

#include "eval.h"
#include "exec.h"
#include "file.h"
#include "file_cache.h"
#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "freshness.h"
#include "func.h"
#include "generation.h"
#include "graph_import.h"
#include "graph_optimization.h"
#include "jobserver.h"
#include "log.h"
#include "monitor.h"
#include "native_observation.h"
#include "ninja.h"
#include "ninja_launcher.h"
#include "parser.h"
#include "process.h"
#include "recursive_cache.h"
#include "regen.h"
#include "regen_dump.h"
#include "rule.h"
#include "session.h"
#include "stats.h"
#include "stmt.h"
#include "stringprintf.h"
#include "strutil.h"
#include "symtab.h"
#include "timeutil.h"
#include "var.h"

struct InvocationArguments {
  char** value = nullptr;
};
#define g_argv (SessionService<InvocationArguments>().value)

static volatile sig_atomic_t build_child = 0;
static volatile sig_atomic_t build_signal = 0;

static void ForwardBuildSignal(int sig) {
  build_signal = sig;
  if (build_child > 0)
    kill(-build_child, sig);
}

// Keep the lock in the supervisor until the child (including Ninja's launcher)
// has finished. Neither recipes nor restarted generation processes inherit it.
static int BuildSubprocess(std::vector<char*> args, const char* directory) {
  if (build_signal)
    return 128 + build_signal;
  sigset_t signals, previous;
  sigemptyset(&signals);
  for (int sig : {SIGHUP, SIGINT, SIGTERM})
    sigaddset(&signals, sig);
  if (sigprocmask(SIG_BLOCK, &signals, &previous) != 0)
    PERROR("block build signals");
  pid_t child = fork();
  if (child < 0)
    PERROR("fork build");
  if (child == 0) {
    if (setpgid(0, 0) != 0)
      PERROR("create build process group");
    for (int sig : {SIGHUP, SIGINT, SIGTERM})
      signal(sig, SIG_DFL);
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    if (directory && chdir(directory) != 0)
      PERROR("chdir build subprocess");
    args.push_back(nullptr);
    execvp(args[0], args.data());
    PERROR("execute build subprocess");
  }
  // The child may reach exec before the parent; either side can establish the
  // same group. Signals target the whole build, including parse-time shells.
  if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH)
    PERROR("set build process group");
  build_child = child;
  sigprocmask(SIG_SETMASK, &previous, nullptr);
  int status;
  if (HANDLE_EINTR(waitpid(child, &status, 0)) < 0)
    PERROR("wait for build subprocess");
  build_child = 0;
  if (build_signal)
    return 128 + build_signal;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int RunGeneratedNinja(GenerationReader& generation,
                             const std::string& launcher,
                             int argc,
                             char** argv);

static int HandleBuild(int argc, char** argv) {
  std::string executable = GetExecutablePath();
  std::vector<char*> generation = {executable.data(),
                                   const_cast<char*>("--ninja"),
                                   const_cast<char*>("--regen")};
  generation.insert(generation.end(), argv + 2, argv + argc);
  generation.push_back(nullptr);
  g_flags.Parse(generation.size() - 1, generation.data());
  generation.pop_back();
  if (g_flags.explain)
    ERROR("use omktr --ninja --regen --explain TARGET to inspect a build");
  if (g_flags.is_question || g_flags.is_touch || g_flags.is_syntax_check_only ||
      g_flags.generate_empty_ninja || g_flags.dump_kati_stamp) {
    ERROR(
        "build does not support question, touch, syntax-only, empty-graph, "
        "or stamp-dump modes");
  }
  if (!g_flags.ninja_dir) {
    g_flags.ninja_dir = ".openmktr";
    generation.push_back(const_cast<char*>("--ninja_dir=.openmktr"));
  }
  char invocation_dir[PATH_MAX];
  if (!getcwd(invocation_dir, sizeof(invocation_dir)))
    PERROR("getcwd build");
  if (g_flags.working_dir && chdir(g_flags.working_dir) != 0)
    PERROR("chdir build");

  // One lock per recipe working directory also serializes Ninja's log files
  // and builds with different suffixes or output directories. Never unlink it:
  // waiters must continue locking the same inode after a previous build exits.
  int lock_fd = open(".openmktr.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (lock_fd < 0)
    PERROR("open build lock");
  struct flock lock = {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  if (HANDLE_EINTR(fcntl(lock_fd, F_SETLKW, &lock)) < 0)
    PERROR("lock build");

  FILE* graph = fopen(GetNinjaFilename().c_str(), "r");
  if (graph) {
    char header[64];
    const bool generated = fgets(header, sizeof(header), graph) &&
                           HasPrefix(header, "# Generated by kati ");
    fclose(graph);
    if (!generated)
      ERROR("refusing to overwrite unrelated graph: %s",
            GetNinjaFilename().c_str());
  } else if (errno != ENOENT) {
    PERROR("read existing build graph");
  }

  struct sigaction action = {};
  action.sa_handler = ForwardBuildSignal;
  sigemptyset(&action.sa_mask);
  for (int sig : {SIGHUP, SIGINT, SIGTERM})
    if (sigaction(sig, &action, nullptr) != 0)
      PERROR("install build signal handler");

  int status;
  {
    ScopedTimeReporter tr("graph validation and generation time");
    status = BuildSubprocess(generation, invocation_dir);
  }
  if (status == 0) {
    std::string jobs = "-j" + std::to_string(g_flags.num_jobs);
    struct stat jobserver;
    const bool inherit_fifo =
        !g_flags.num_jobs_specified && !g_flags.is_dry_run &&
        g_flags.remote_num_jobs <= 0 && !g_flags.ninja_jobserver_fifo.empty() &&
        stat(g_flags.ninja_jobserver_fifo.c_str(), &jobserver) == 0 &&
        S_ISFIFO(jobserver.st_mode);
    std::vector<char*> ninja;
    if (!inherit_fifo)
      ninja.push_back(jobs.data());
    std::string load_option = g_flags.LoadLimitOption();
    if (g_flags.load_limit >= 0)
      ninja.push_back(load_option.data());
    if (g_flags.is_dry_run)
      ninja.push_back(const_cast<char*>("-n"));
    if (g_flags.keep_going)
      ninja.push_back(const_cast<char*>("-k0"));
    ScopedTimeReporter tr("Ninja execution time");
    GenerationReader reader(CurrentGenerationPointer());
    if (!reader.valid())
      ERROR("invalid generation; regenerate the Ninja graph");
    status = RunGeneratedNinja(
        reader, std::string(Basename(GetNinjaShellScriptFilename())),
        ninja.size(), ninja.data());
  }
  close(lock_fd);
  ReportAllStats();
  return status;
}

namespace {

bool StatPath(const char* path, struct stat* result) {
  int status;
  do {
    status = stat(path, result);
  } while (status < 0 && errno == EINTR);
  return status == 0;
}

bool IsNewer(const struct stat& input,
             const struct stat& output,
             bool low_resolution) {
  return InputIsNewer(FileStamp::FromStat(input), FileStamp::FromStat(output),
                      EdgeKind::Normal, low_resolution);
}

int HandleNewerInputs(int argc, char** argv) {
  if (argc < 3)
    return 2;

  struct stat output_stat;
  const bool output_exists = StatPath(argv[2], &output_stat);
  std::unordered_set<std::string> low_resolution_inputs;
  int separator = argc;
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      separator = i;
      for (int j = i + 1; j < argc; ++j)
        low_resolution_inputs.insert(argv[j]);
      break;
    }
  }

  bool first = true;
  for (int i = 3; i < separator; ++i) {
    struct stat input_stat;
    if (!StatPath(argv[i], &input_stat) ||
        (output_exists && !IsNewer(input_stat, output_stat,
                                   low_resolution_inputs.find(argv[i]) !=
                                       low_resolution_inputs.end())))
      continue;
    if (!first && putchar(' ') == EOF)
      return 1;
    if (fputs(argv[i], stdout) == EOF)
      return 1;
    first = false;
  }
  return fflush(stdout) == 0 ? 0 : 1;
}

int HandleLowResolutionUpToDate(int argc, char** argv) {
  if (argc < 5)
    return 1;

  char* end = nullptr;
  errno = 0;
  const unsigned long normal_count = strtoul(argv[3], &end, 10);
  if (errno != 0 || end == argv[3] || *end != '\0')
    return 1;
  errno = 0;
  const unsigned long low_resolution_count = strtoul(argv[4], &end, 10);
  if (errno != 0 || end == argv[4] || *end != '\0' ||
      normal_count > (unsigned long)(argc - 5) ||
      low_resolution_count != (unsigned long)(argc - 5) - normal_count)
    return 1;

  const int normal_end = 5 + (int)normal_count;
  std::unordered_set<std::string> low_resolution_inputs;
  for (int i = normal_end; i < argc; ++i)
    low_resolution_inputs.insert(argv[i]);

  struct stat output_stat;
  if (!StatPath(argv[2], &output_stat))
    return 1;

  for (int i = 5; i < normal_end; ++i) {
    struct stat input_stat;
    if (!StatPath(argv[i], &input_stat))
      return 1;
    const bool low_resolution =
        low_resolution_inputs.find(argv[i]) != low_resolution_inputs.end();
    if (IsNewer(input_stat, output_stat, low_resolution))
      return 1;
  }
  return 0;
}

int HandleIntermediateCurrent(const char* output,
                              const char* path,
                              bool consumer = false) {
  const char* goal_environment = getenv("KATI_NINJA_GOALS");
  struct stat ignored;
  if (!goal_environment || StatPath(output, &ignored) != consumer)
    return 1;
  std::unordered_set<std::string> goals;
  std::string_view encoded(goal_environment);
  while (!encoded.empty()) {
    const size_t colon = encoded.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 9)
      return 1;
    size_t length = 0;
    for (size_t i = 0; i < colon; ++i) {
      if (encoded[i] < '0' || encoded[i] > '9')
        return 1;
      length = length * 10 + encoded[i] - '0';
    }
    encoded.remove_prefix(colon + 1);
    if (length > encoded.size())
      return 1;
    std::string goal(encoded.substr(0, length));
    NormalizePath(&goal);
    goals.insert(goal);
    encoded.remove_prefix(length);
  }
  if (goals.empty())
    return 1;
  std::string payload, version, roots, records, state_index;
  if (!ReadGraphCache(path, &payload))
    return 1;
  std::string_view data(payload);
  if (!ConsumeGraphString(&data, &version) ||
      version != "intermediate-proof-v2" ||
      !ConsumeGraphString(&data, &roots) ||
      !ConsumeGraphString(&data, &records) ||
      !ConsumeGraphString(&data, &state_index) || !data.empty())
    return 1;
  std::string state_records;
  if (!ReadGraphCache(state_index, &state_records))
    return 1;
  std::unordered_map<std::string, std::string> states;
  data = state_records;
  if (!ConsumeGraphString(&data, &version) ||
      version != "intermediate-states-v1")
    return 1;
  while (!data.empty()) {
    std::string name, state;
    if (!ConsumeGraphString(&data, &name) ||
        !ConsumeGraphString(&data, &state) ||
        !states.emplace(name, state).second)
      return 1;
  }
  struct Node {
    bool intermediate;
    bool always;
    std::vector<std::string> deps, order_only;
  };
  std::unordered_map<std::string, Node> nodes;
  data = records;
  while (!data.empty()) {
    std::string record, name, flags, deps, order_only;
    if (!ConsumeGraphString(&data, &record))
      return 1;
    std::string_view fields(record);
    if (!ConsumeGraphString(&fields, &name) ||
        !ConsumeGraphString(&fields, &flags) || flags.size() != 2 ||
        flags.find_first_not_of("01") != std::string::npos ||
        !ConsumeGraphString(&fields, &deps) ||
        !ConsumeGraphString(&fields, &order_only) || !fields.empty())
      return 1;
    Node node{flags[0] == '1', flags[1] == '1', {}, {}};
    std::string normalized = name;
    NormalizePath(&normalized);
    if (node.intermediate && goals.count(normalized))
      return 1;
    for (auto pair : {std::make_pair(&deps, &node.deps),
                      std::make_pair(&order_only, &node.order_only)}) {
      std::string_view inputs(*pair.first);
      while (!inputs.empty()) {
        std::string input;
        if (!ConsumeGraphString(&inputs, &input))
          return 1;
        pair.second->push_back(std::move(input));
      }
    }
    if (!nodes.emplace(name, std::move(node)).second)
      return 1;
  }
  std::unordered_set<std::string> visiting;
  auto current = [&](auto&& self, const std::string& name,
                     const struct stat& timestamp) -> bool {
    auto found = nodes.find(name);
    if (found == nodes.end() || found->second.always ||
        !visiting.insert(name).second)
      return false;
    auto state = states.find(name);
    if (state != states.end()) {
      std::string expected, successful;
      if (!ReadNinjaRecipeState(state->second, &expected) ||
          !ReadNinjaRecipeState(
              NinjaRecipeStateOwner(state->second) + ".success", &successful) ||
          expected != successful)
        return false;
    }
    for (const std::string& input : found->second.deps) {
      auto child = nodes.find(input);
      if (child == nodes.end() || child->second.always)
        return false;
      struct stat input_timestamp;
      if (!StatPath(input.c_str(), &input_timestamp)) {
        if (!child->second.intermediate ||
            (child->second.deps.empty() && !states.count(input)) ||
            !self(self, input, timestamp))
          return false;
      } else if (IsNewer(input_timestamp, timestamp, false) ||
                 !self(self, input, input_timestamp)) {
        return false;
      }
    }
    for (const std::string& input : found->second.order_only) {
      struct stat input_timestamp;
      if (!StatPath(input.c_str(), &input_timestamp) ||
          !self(self, input, input_timestamp))
        return false;
    }
    visiting.erase(name);
    return true;
  };
  data = roots;
  if (data.empty())
    return 1;
  while (!data.empty()) {
    std::string root;
    struct stat timestamp;
    if (!ConsumeGraphString(&data, &root) ||
        !StatPath(root.c_str(), &timestamp) ||
        !current(current, root, timestamp))
      return 1;
  }
  return 0;
}

}  // namespace

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
       "KATI?=omktr\n"
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
  if (g_flags.is_question && !g_flags.explain)
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
  if (g_flags.strict_compat)
    append_option("--strict-compat");

  // Wrappers such as QEMU's Makefile forward this option to Ninja. Replace
  // inherited job limits with the effective limit selected for this process.
  std::string without_jobs;
  bool skip_job_count = false;
  bool skip_load_count = false;
  for (std::string_view token : WordScanner(makeflags_value)) {
    if (skip_load_count) {
      skip_load_count = false;
      if (!token.empty() &&
          (isdigit(static_cast<unsigned char>(token[0])) || token[0] == '.'))
        continue;
    }
    if (token == "-l") {
      skip_load_count = true;
      continue;
    }
    if (HasPrefix(token, "-l") || token == "--load-average" ||
        token == "--max-load" || HasPrefix(token, "--load-average=") ||
        HasPrefix(token, "--max-load="))
      continue;
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
  if (g_flags.load_limit_set) {
    // Append even if an older clustered option contains the same limit:
    // the effective setting, including a bare -l reset, must come last.
    makeflags_value += " " + g_flags.LoadLimitOption();
  }
  if (getenv("KATI_NINJA_LOAD_LIMIT")) {
    // Preserve a child's explicit override or reset for grandchildren.
    if (g_flags.load_limit < 0)
      unsetenv("KATI_NINJA_LOAD_LIMIT");
    else
      setenv("KATI_NINJA_LOAD_LIMIT",
             g_flags.LoadLimitOption().substr(2).c_str(), 1);
  }

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
  Var* mflags = Intern("MFLAGS").PeekGlobalVar();
  if (!mflags->IsDefined() || mflags->Origin() == VarOrigin::DEFAULT ||
      mflags->Origin() == VarOrigin::ENVIRONMENT) {
    SetVar("MFLAGS=" + mflags_value, VarOrigin::ENVIRONMENT, nullptr,
           Loc("*bootstrap*", 0));
  }
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
  double start_time = GetWallTime();
  LOG_STAT("graph optimization settings: %s",
           g_flags.GraphOptimizationIdentity().c_str());
  if (g_flags.import_root) {
    std::vector<std::string> files;
    if (g_flags.is_dry_run || g_flags.is_touch || g_flags.is_question ||
        g_flags.keep_going || g_flags.load_limit_set ||
        !StaticImportSources(&files))
      return 3;
  }
  RecursiveCache recursive_cache(orig_args);
  if (!g_flags.explain && recursive_cache.Reuse())
    return 0;

  const bool has_stdin_makefile =
      std::any_of(g_flags.makefiles.begin(), g_flags.makefiles.end(),
                  [](const char* name) { return strcmp(name, "-") == 0; });
  // Stdin has no persistent timestamp. Even a real file named '-' cannot
  // establish that the stream supplied to this invocation is unchanged.
  if (!has_stdin_makefile && g_flags.generate_ninja &&
      (g_flags.regen || g_flags.dump_kati_stamp || g_flags.explain)) {
    ScopedTimeReporter tr("regen check time");
    const bool needs_regen = NeedsRegen(start_time, orig_args);
    if (g_flags.explain) {
      printf("Regeneration: %s\n", needs_regen ? "required (see diagnostics)"
                                               : "recorded inputs unchanged");
    } else if (!needs_regen) {
      fprintf(stderr, "No need to regenerate ninja file\n");
      return 0;
    }
    if (g_flags.dump_kati_stamp) {
      printf("Need to regenerate ninja file\n");
      return 0;
    }
    ClearGlobCache();
  }
  if (g_flags.explain) {
    if (!g_flags.generate_ninja)
      printf("Regeneration: not applicable in direct mode\n");
    else if (has_stdin_makefile)
      printf("Regeneration: required because a Makefile is read from stdin\n");
    printf("Explanation uses Make freshness decisions; recipes are not run.\n");
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
      if (name == "KATI_GRAPH_SETTINGS" || name == "KATI_RUNTIME_CONTEXT_FD" ||
          name == "KATI_NINJA_GOALS" || name == "KATI_NINJA_DEFAULT_GOALS")
        continue;
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

      std::vector<std::unique_ptr<DepNode>> include_storage;
      std::vector<NamedDepNode> include_nodes;
      std::unordered_map<std::string, std::string> include_observations;
      bool include_observations_valid = false;

      {
        ScopedFrame frame(
            ev.Enter(FrameType::PHASE, "*remake included makefiles*", Loc()));

        ScopedTimeReporter tr("remake included makefiles time");

        // Included makefiles may create their own parent directories. Adding
        // synthetic directory edges here can introduce a cycle through a
        // project's prepare target before the include has been generated.
        ev.RefreshVpath();
        ScopedFileObservations observations(
            g_flags.GraphOptimizationEnabled("includes"));
        MakeDep(&ev, ev.rules(), ev.rule_vars(), include_targets,
                &include_nodes, &include_storage, false);
        include_observations_valid = observations.valid();
        include_observations = observations.Identities();
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
        // The next invocation must remake includes before it can establish
        // graph validity, even if the include itself has not changed yet.
        MakefileCacheManager::Get().RequireIncludeEvaluation();
        const DepNode* makefile_node = include.second;
        bool unconditional = false;
        for (size_t group = 0;
             group < makefile_node->DoubleColon().has_prerequisites.size();
             ++group) {
          if (!makefile_node->DoubleColon().has_prerequisites[group] &&
              std::find(makefile_node->DoubleColon().for_cmd.begin(),
                        makefile_node->DoubleColon().for_cmd.end(),
                        group) != makefile_node->DoubleColon().for_cmd.end())
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
          include_result = Exec(include_remake_nodes, &ev, true,
                                g_flags.generate_ninja, false);
          if (include_result.needs_build)
            InvalidateEvaluationFilesystemQueries();
          if (g_flags.explain && include_result.needs_build)
            ERROR(
                "--explain: generated Makefiles need rebuilding; run a "
                "normal build before explaining the resulting rules");
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
          // A complete, unchanged explicit include graph can certify that
          // remaking it is a no-op. Recording every node in the normal stamp
          // binds this proof to its producing rules and dependency identities.
          // Implicit/VPATH searches need negative probes before admission.
          if (g_flags.generate_ninja && !g_flags.explain &&
              g_flags.GraphOptimizationEnabled("includes") &&
              !include_result.needs_build && include_observations_valid &&
              deferred_include_nodes.empty() && ev.vpaths().empty() &&
              !MakefileCacheManager::Get().has_observable_evaluation()) {
            std::vector<const DepNode*> pending;
            for (const auto& root : include_remake_nodes)
              pending.push_back(root.second);
            std::unordered_set<const DepNode*> visited;
            std::vector<std::string> files;
            bool eligible = true;
            while (!pending.empty() && eligible) {
              const DepNode* node = pending.back();
              pending.pop_back();
              if (!visited.insert(node).second)
                continue;
              struct stat st;
              eligible =
                  !node->is_phony && !node->is_notparallel &&
                  !node->vpath_provider.IsValid() &&
                  node->implicit_outputs.empty() && node->validations.empty() &&
                  node->DoubleColon().for_cmd.empty() &&
                  node->low_resolution_inputs.empty() &&
                  stat(node->output.c_str(), &st) == 0 && S_ISREG(st.st_mode);
              if (!eligible)
                break;
              files.push_back(node->output.str());
              for (const auto& input : node->deps)
                pending.push_back(input.second);
              for (const auto& input : node->order_onlys)
                pending.push_back(input.second);
            }
            for (const auto& observation : include_observations)
              if (observation.second == "unreadable" ||
                  GetFileIdentity(observation.first) != observation.second)
                eligible = false;
            if (eligible) {
              auto& cache = MakefileCacheManager::Get();
              for (const auto& observation : include_observations)
                cache.AddExtraFileDep(observation.first);
              for (const std::string& file : files)
                cache.AddExtraFileDep(file);
              cache.CertifyIncludeEvaluation();
              LOG_STAT("include certificate: %zu nodes", files.size());
            }
          }
          if (!deferred_include_nodes.empty()) {
            const bool touch_mode = g_flags.is_touch;
            g_flags.is_touch = false;
            const ExecResult deferred_result =
                Exec(deferred_include_nodes, &ev, true, false, false);
            if (deferred_result.needs_build)
              InvalidateEvaluationFilesystemQueries();
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

    std::vector<std::unique_ptr<DepNode>> node_storage;
    std::vector<NamedDepNode> nodes;

    {
      ScopedFrame frame(
          ev.Enter(FrameType::PHASE, "*dependency analysis*", Loc()));

      ScopedTimeReporter tr("make dep time");
      ScopedFileObservations observations(
          g_flags.GraphOptimizationEnabled("evaluation"));

      ev.RefreshVpath();
      MakeDep(&ev, ev.rules(), ev.rule_vars(), targets, &nodes, &node_storage);
    }

    if (g_flags.is_syntax_check_only) {
      ev.Finish();

      for (Stmt* stmt : bootstrap_asts)
        delete stmt;

      return 0;
    }

    if (g_flags.generate_ninja && !g_flags.explain) {
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
      if (name.str() == "KATI_GRAPH_SETTINGS")
        continue;

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

      if (g_flags.explain && g_flags.generate_ninja)
        ev.set_avoid_io(true);
      const ExecResult execution = Exec(nodes, &ev, !g_flags.explain);

      if (g_flags.explain) {
        ev.set_avoid_io(false);
        ev.Finish();
        for (Stmt* stmt : bootstrap_asts)
          delete stmt;
        return execution.failed ? 1 : 0;
      }

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
      recursive_cache.Save(nodes, &ev);
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

namespace {
// Keep launcher resources in one native parent. Its child closes the lease
// and FIFO descriptors on exec; cancellation is forwarded before cleanup.
struct NinjaLaunchResources {
  int lease = -1;
  int fifo_fd = -1;
  std::string fifo, load_state;
  ~NinjaLaunchResources() {
    if (lease >= 0)
      close(lease);
    if (fifo_fd >= 0)
      close(fifo_fd);
    if (!fifo.empty())
      unlink(fifo.c_str());
    if (!load_state.empty())
      unlink(load_state.c_str());
  }
  void CreateJobserver() {
    const char* inherited = getenv("KATI_JOBSERVER_FIFO");
    if (inherited && *inherited)
      return;
    const char* jobs = getenv("KATI_JOBS");
    char* end = nullptr;
    errno = 0;
    unsigned long count = jobs ? strtoul(jobs, &end, 10) : 0;
    if (!jobs || !*jobs || *end || errno || !count || *jobs == '-')
      count = 1;
    const char* temp = getenv("TMPDIR");
    const std::string base = std::string(temp && *temp ? temp : "/tmp") +
                             "/kati-jobserver-" + std::to_string(getpid());
    for (size_t i = 0; i < 1024; ++i) {
      const std::string path = base + (i ? "-" + std::to_string(i) : "");
      if (mkfifo(path.c_str(), 0600) == 0) {
        fifo = path;
        break;
      }
      if (errno != EEXIST)
        PERROR("cannot create jobserver FIFO %s", path.c_str());
    }
    if (fifo.empty())
      ERROR("cannot create jobserver FIFO: too many name collisions");
    fifo_fd = open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0)
      PERROR("open jobserver FIFO");
    // A nonblocking fill also bounds oversized job counts to the transport's
    // capacity instead of hanging before any reader has started.
    const std::string tokens(1024, 'x');
    unsigned long written = 0;
    while (written < count) {
      const ssize_t n = HANDLE_EINTR(
          write(fifo_fd, tokens.data(),
                std::min<unsigned long>(count - written, tokens.size())));
      if (n < 0 && errno == EAGAIN)
        break;
      if (n <= 0)
        PERROR("initialize jobserver FIFO");
      written += n;
    }
    setenv("KATI_JOBSERVER_FIFO", fifo.c_str(), 1);
    setenv("KATI_JOBS", std::to_string(written).c_str(), 1);
  }
};
}  // namespace

static int HandleRunNinja(int argc, char** argv, bool supervise = false) {
  NinjaLaunchResources resources;
  struct sigaction action = {};
  action.sa_handler = ForwardBuildSignal;
  sigemptyset(&action.sa_mask);
  for (int sig : {SIGHUP, SIGINT, SIGTERM})
    if (sigaction(sig, &action, nullptr) != 0)
      PERROR("install Ninja signal handler");
  if (const char* lease = getenv("OMKTR_GENERATION_LEASE_FD")) {
    char* end = nullptr;
    const long fd = strtol(lease, &end, 10);
    if (*lease && !*end && fd >= 20 && fd <= INT_MAX) {
      resources.lease = static_cast<int>(fd);
      if (fcntl(resources.lease, F_SETFD, FD_CLOEXEC) != 0)
        PERROR("retain generation lease");
    }
    unsetenv("OMKTR_GENERATION_LEASE_FD");
  }
  resources.CreateJobserver();
  // argv: tool, helper, generation-time limit (empty means disabled), Ninja,
  // then Ninja arguments. Keep targets after -- completely opaque.
  double limit = *argv[2] ? ParseLoadLimit(argv[2]) : -1;
  bool load_set = *argv[2];
  std::vector<std::string> arguments;
  bool options = true;
  bool known_goals = true;
  std::string ninja_file = "build.ninja";
  std::string goals;
  size_t option_end = static_cast<size_t>(-1);
  for (int i = 4; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const bool separator = options && arg == "--";
    if (options && (arg == "--" || HasPrefix(arg, "-t"))) {
      if (HasPrefix(arg, "-t"))
        known_goals = false;
      option_end = arguments.size();
      options = false;
    }
    if (options && (arg == "-f" || arg == "-C" || arg == "-j" || arg == "-k" ||
                    arg == "-d" || arg == "-w")) {
      if (arg == "-C")
        known_goals = false;
      arguments.emplace_back(arg);
      if (++i == argc)
        ERROR("Missing Ninja option operand: %s", arguments.back().c_str());
      arguments.emplace_back(argv[i]);
      if (arg == "-f")
        ninja_file = argv[i];
      continue;
    }
    if (options &&
        (arg == "-l" || arg == "--load-average" || arg == "--max-load")) {
      limit = -1;
      load_set = true;
      if (arg == "-l" && i + 1 < argc &&
          (isdigit(static_cast<unsigned char>(argv[i + 1][0])) ||
           argv[i + 1][0] == '.'))
        limit = ParseLoadLimit(argv[++i]);
      continue;
    }
    if (options && (HasPrefix(arg, "-l") || HasPrefix(arg, "--load-average=") ||
                    HasPrefix(arg, "--max-load="))) {
      const size_t start = arg[1] == 'l' ? 2 : arg.find('=') + 1;
      limit = ParseLoadLimit(argv[i] + start);
      load_set = true;
      continue;
    }
    if (!separator && (!options || arg.empty() || arg.front() != '-'))
      goals += std::to_string(arg.size()) + ":" + std::string(arg);
    if (options && HasPrefix(arg, "-C"))
      known_goals = false;
    if (options && HasPrefix(arg, "-f") && arg.size() > 2)
      ninja_file = std::string(arg.substr(2));
    arguments.emplace_back(arg);
  }
  const char* defaults = getenv("KATI_NINJA_DEFAULT_GOALS");
  if (goals.empty()) {
    if (defaults)
      goals = defaults;
  }
  if (defaults && *defaults && known_goals && !goals.empty())
    setenv("KATI_NINJA_GOALS", goals.c_str(), 1);
  else
    unsetenv("KATI_NINJA_GOALS");
  if (option_end == static_cast<size_t>(-1))
    option_end = arguments.size();
  const std::string value = limit < 0 ? "" : StringPrintf("%.17g", limit);
  if (limit >= 0)
    setenv("KATI_NINJA_LOAD_LIMIT", value.c_str(), 1);
  else
    unsetenv("KATI_NINJA_LOAD_LIMIT");
  const char* inherited_flags = getenv("MAKEFLAGS");
  const std::string makeflags =
      std::string(inherited_flags ? inherited_flags : "") + " -l" + value;
  if (load_set)
    setenv("MAKEFLAGS", makeflags.c_str(), 1);

  std::string& state = resources.load_state;
  if (limit >= 0) {
    const char* temp = getenv("TMPDIR");
    state = std::string(temp && *temp ? temp : "/tmp") + "/omktr-load-XXXXXX";
    const int fd = mkstemp(state.data());
    if (fd < 0)
      PERROR("create Ninja load state failed");
    const char initial = 0;
    if (write(fd, &initial, 1) != 1 || close(fd) != 0)
      PERROR("initialize Ninja load state failed");
    setenv("KATI_NINJA_LOAD_STATE", state.c_str(), 1);
    // Load admission belongs to our helpers. Disable Ninja's independent
    // sampler so a failed sample cannot evade our fallback and so helpers
    // waiting for load do not count as active recipes in a second scheduler.
    arguments.insert(arguments.begin() + option_end++, "-l0");
    if (limit == 0)
      arguments.insert(arguments.begin() + option_end++, "-j1");
  } else {
    unsetenv("KATI_NINJA_LOAD_STATE");
  }
  if (const char* reserved = getenv("KATI_JOBSERVER_RESERVED")) {
    if (!strcmp(reserved, "1"))
      arguments.insert(arguments.begin() + option_end, "-j1");
  }
  if (known_goals)
    PrepareRuntimeContexts(
        RuntimeContextManifestPath(ResolveGenerationArtifact(ninja_file)));
  else
    unsetenv("KATI_RUNTIME_CONTEXT_FD");
  if (!supervise && state.empty() && resources.fifo.empty() &&
      resources.lease < 0) {
    // The ordinary path needs no additional supervisor or process boundary.
    std::vector<char*> child = {argv[3]};
    for (std::string& arg : arguments)
      child.push_back(arg.data());
    child.push_back(nullptr);
    execvp(child.front(), child.data());
    PERROR("exec Ninja failed");
  }
  std::vector<char*> child = {argv[3]};
  for (const std::string& arg : arguments)
    child.push_back(const_cast<char*>(arg.c_str()));
  return BuildSubprocess(child, nullptr);
}

static int RunGeneratedNinja(GenerationReader& generation,
                             const std::string& launcher,
                             int argc,
                             char** argv) {
  std::string bytes;
  NinjaLaunchPlan plan;
  if (launcher.empty() || launcher.find('/') != std::string::npos ||
      !ReadGraphCache(generation.directory() + "/" + launcher + ".launch",
                      &bytes) ||
      !plan.Decode(bytes))
    ERROR("invalid generation launch plan; regenerate the Ninja graph");
  plan.ApplyEnvironment(argc, argv);
  std::string graph = generation.directory() + "/" + plan.graph;
  // Preserve caller-created regular wrappers while pinning generated aliases.
  struct stat metadata;
  if (lstat(plan.public_graph.c_str(), &metadata) == 0 &&
      S_ISREG(metadata.st_mode))
    graph = plan.public_graph;
  std::vector<std::string> arguments = {GetExecutablePath(),
                                        "--internal-run-ninja",
                                        plan.load,
                                        plan.ninja,
                                        "-f",
                                        graph};
  if (!plan.remote_jobs.empty())
    arguments.push_back(plan.remote_jobs);
  for (int i = 0; i < argc; ++i)
    arguments.emplace_back(argv[i]);
  std::vector<char*> pointers;
  for (auto& argument : arguments)
    pointers.push_back(argument.data());
  // The same native parent keeps the reader lease until Ninja has exited.
  return HandleRunNinja(pointers.size(), pointers.data(), true);
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

static bool IsWorkspaceAction(const char* argument) {
  for (const char* action :
       {"run", "path", "status", "pack", "unpack", "gc", "list",
        "portage-prepare", "portage-enter", "--help", "-h"}) {
    if (!strcmp(action, argument))
      return true;
  }
  return false;
}

static int Main(int argc, char* argv[]) {
  if (argc >= 4 && !strcmp(argv[1], "--internal-pin-generation")) {
    // A reader can race collection before acquiring its lease. Retry the
    // currently published bundle; every subsequent path is pinned to it.
    auto generation = std::make_unique<GenerationReader>(argv[2]);
    if (!generation->valid()) {
      std::string current = std::string(Dirname(argv[2])) + "/current";
      generation.reset();
      generation = std::make_unique<GenerationReader>(current);
    }
    if (!generation->valid())
      ERROR("invalid generation; regenerate the Ninja graph");
    const std::string directory = generation->directory();
    std::string launcher = directory + "/" + argv[3];
    setenv("OMKTR_PINNED_GENERATION", directory.c_str(), 1);
    std::vector<char*> arguments = {const_cast<char*>("/bin/sh"),
                                    launcher.data()};
    arguments.insert(arguments.end(), argv + 4, argv + argc);
    arguments.push_back(nullptr);
    const std::string lease = std::to_string(generation->InheritLease());
    setenv("OMKTR_GENERATION_LEASE_FD", lease.c_str(), 1);
    execv(arguments[0], arguments.data());
    PERROR("execute pinned generation launcher");
  }
  if (argc == 3 && !strcmp(argv[1], "--internal-evaluate-recipe"))
    return RunRuntimeRecipe(argv[2]);
  if (argc == 4 && strcmp(argv[1], "--internal-native-find") == 0 &&
      (strcmp(argv[3], "0") == 0 || strcmp(argv[3], "1") == 0))
    return NativeFindWorker(argv[2], strcmp(argv[3], "1") == 0);
  g_argv = argv;
  // This is live per-recipe supervision state, never a recursive make input.
  unsetenv("KATI_RECIPE_CHANGED");
  unsetenv("KATI_RECIPE_STATE_PATH");
  // Recursive invocations isolate omktr from a target project's runtime
  // libraries while it is being loaded. Restore that runtime environment
  // after the loader has started this process; recipes executed by this child
  // still need the target library path.
  if (const char* saved_ld_library_path =
          getenv("KATI_SAVED_LD_LIBRARY_PATH")) {
    setenv("LD_LIBRARY_PATH", saved_ld_library_path, 1);
    unsetenv("KATI_SAVED_LD_LIBRARY_PATH");
  }

  if (argc >= 2) {
    if (argc == 3 && !strcmp(argv[1], "monitor") &&
        (!strcmp(argv[2], "start") || !strcmp(argv[2], "status") ||
         !strcmp(argv[2], "stop")))
      return MonitorCommand(argv[2]);
    if (!strcmp(argv[1], "write-if-changed") && argc >= 4)
      return WriteIfChanged(argc - 2, argv + 2);
    // Keep `omktr workspace` available as an ordinary Make target. The
    // frontend is selected only by an explicit workspace action or help.
    if (argc >= 3 && !strcmp(argv[1], "workspace") &&
        IsWorkspaceAction(argv[2])) {
      const std::string executable = GetExecutablePath();
      const std::string directory = executable.substr(0, executable.rfind('/'));
      std::string helper = directory + "/omktr-workspace";
      const bool installed_helper = access(helper.c_str(), X_OK) == 0;
      if (!installed_helper)
        helper = directory + "/tools/omktr_workspace.py";
      if (access(helper.c_str(), R_OK) != 0)
        ERROR("workspace frontend is missing beside %s", executable.c_str());
      setenv("OMKTR_WORKSPACE_TOOL", executable.c_str(), 1);
      std::vector<char*> arguments;
      if (!installed_helper)
        arguments.push_back(const_cast<char*>("python3"));
      arguments.push_back(helper.data());
      arguments.insert(arguments.end(), argv + 2, argv + argc);
      arguments.push_back(nullptr);
      execvp(arguments.front(), arguments.data());
      PERROR("execute workspace frontend");
    } else if (!strcmp(argv[1], "--internal-launch-generation") && argc >= 4) {
      auto generation = std::make_unique<GenerationReader>(argv[2]);
      if (!generation->valid()) {
        const std::string current = std::string(Dirname(argv[2])) + "/current";
        generation.reset();
        generation = std::make_unique<GenerationReader>(current);
      }
      if (!generation->valid())
        ERROR("invalid generation; regenerate the Ninja graph");
      return RunGeneratedNinja(*generation, argv[3], argc - 4, argv + 4);
    } else if (!strcmp(argv[1], "--internal-run-ninja") && argc >= 4) {
      return HandleRunNinja(argc, argv);
    } else if ((!strcmp(argv[1], "--internal-run-recipe") ||
                !strcmp(argv[1], "--internal-run-recursive-recipe") ||
                !strcmp(argv[1], "--internal-run-recipe-file") ||
                !strcmp(argv[1], "--internal-run-recursive-recipe-file")) &&
               (argc == 5 || argc == 6 || argc == 7)) {
      std::string script;
      if (!strcmp(argv[1], "--internal-run-recipe-file") ||
          !strcmp(argv[1], "--internal-run-recursive-recipe-file")) {
        if (!ReadGraphCache(argv[4], &script))
          ERROR("missing or corrupt recipe script; regenerate the Ninja graph");
        argv[4] = script.data();
      }
      ScopedBuildSignals signals;
      const int load_slot = AcquireNinjaLoadSlot();
      std::string recipe_state;
      if (argc == 7) {
        if (!ReadNinjaRecipeState(argv[6], &recipe_state))
          ERROR(
              "missing or invalid workspace recipe state; regenerate the Ninja "
              "graph");
        std::string previous;
        const std::string owner = NinjaRecipeStateOwner(argv[6]);
        const std::string success_file = owner + ".success";
        const bool recorded = ReadNinjaRecipeState(success_file, &previous);
        if (!recorded && Exists(success_file))
          ERROR(
              "invalid successful workspace recipe state; regenerate the "
              "workspace");
        setenv("KATI_RECIPE_CHANGED",
               recorded && previous != recipe_state ? "1" : "0", 1);
        setenv("KATI_RECIPE_STATE_PATH", owner.c_str(), 1);
        unlink((owner + ".reads-pending").c_str());
      } else {
        unsetenv("KATI_RECIPE_STATE_PATH");
      }
      const int status = RunCommand(
          argv[2], argv[3], argv[4], RedirectStderr::NONE, nullptr,
          (!strcmp(argv[1], "--internal-run-recipe") ||
           !strcmp(argv[1], "--internal-run-recipe-file")),
          [](std::string_view output) {
            fwrite(output.data(), 1, output.size(), stdout);
            fflush(stdout);
          },
          -1, nullptr, nullptr, argc >= 6 ? argv[5] : "");
      if (load_slot >= 0)
        close(load_slot);
      if (GetBuildSignal() != 0)
        return 128 + GetBuildSignal();
      if (argc == 7 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        const std::string owner = NinjaRecipeStateOwner(argv[6]);
        CommitRuntimeRecipeReads(owner, recipe_state);
        WriteNinjaRecipeState(owner + ".success", recipe_state);
      }
      return WIFEXITED(status)     ? WEXITSTATUS(status)
             : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                   : 1;
    } else if (!strcmp(argv[1], "--realpath")) {
      HandleRealpath(argc - 2, argv + 2);
      return 0;
    } else if (!strcmp(argv[1], "--internal-newer-inputs")) {
      return HandleNewerInputs(argc, argv);
    } else if (!strcmp(argv[1], "--internal-output-stamp") && argc == 3) {
      puts(GetOutputStamp(argv[2]).c_str());
      return 0;
    } else if (!strcmp(argv[1], "--internal-delete-changed-output") &&
               argc == 4) {
      DeleteChangedOutput(argv[2], argv[3]);
      return 0;
    } else if (!strcmp(argv[1], "--internal-low-resolution-up-to-date")) {
      return HandleLowResolutionUpToDate(argc, argv);
    } else if (!strcmp(argv[1], "--internal-intermediate-current") &&
               argc == 4) {
      return HandleIntermediateCurrent(argv[2], argv[3]);
    } else if (!strcmp(argv[1], "--internal-intermediate-consumer-current") &&
               argc == 4) {
      return HandleIntermediateCurrent(argv[2], argv[3], true);
    } else if (!strcmp(argv[1], "--file-read")) {
      return HandleFileRead(argc - 2, argv + 2);
    } else if (!strcmp(argv[1], "build")) {
      return HandleBuild(argc, argv);
    } else if (!strcmp(argv[1], "--dump_stamp_tool")) {
      // Unfortunately, this can easily be confused with --dump_kati_stamp,
      // which prints debug info about the stamp while executing a normal kati
      // run. This tool flag only dumps information, and doesn't run the rest of
      // kati.
      return stamp_dump_main(argc, argv);
    }
  }
  g_flags.Parse(argc, argv);
  std::string orig_args;
  for (int i = 0; i < argc; i++) {
    // An explanation checks the same invocation's regeneration stamp.
    if (std::find(g_flags.diagnostic_arg_indices.begin(),
                  g_flags.diagnostic_arg_indices.end(),
                  i) != g_flags.diagnostic_arg_indices.end())
      continue;
    if (std::find(g_flags.explain_arg_indices.begin(),
                  g_flags.explain_arg_indices.end(),
                  i) != g_flags.explain_arg_indices.end())
      continue;
    if (!orig_args.empty())
      orig_args += ' ';
    // Preserve argument boundaries: ["A=x y", "B=z"] and
    // ["A=x", "y B=z"] must invalidate each other's graph.
    orig_args += std::to_string(strlen(argv[i]));
    orig_args += ':';
    orig_args += argv[i];
  }
  char invocation_dir_buf[PATH_MAX];
  if (!getcwd(invocation_dir_buf, sizeof(invocation_dir_buf)))
    PERROR("getcwd");
  const std::string invocation_dir(invocation_dir_buf);
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

int main(int argc, char* argv[]) {
  BuildSession session;
  try {
    return Main(argc, argv);
  } catch (const BuildExit& e) {
    return e.status;
  } catch (const std::exception& e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
