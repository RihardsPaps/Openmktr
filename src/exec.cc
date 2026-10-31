// FORK MODIFICATION NOTICE (2026)
// Own invocation state and publish coherent generations with typed freshness.
// Schedule resolved dependencies with a bounded ready queue. Preserve serial
// goal order and parent-specific intermediate decisions. Expand recipes under
// one evaluator lock; share process supervision and recursive job leases.
// Changed by the Openmktr fork, maintained by Rihards Paps and Haralds Paps.
// Upstream material retains Apache-2.0 terms. Fork changes are covered by
// MPL-2.0; see docs/LICENSING.md and NOTICE. Original notices follow.

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

#include "exec.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "command.h"
#include "dep.h"
#include "eval.h"
#include "expr.h"
#include "fileutil.h"
#include "flags.h"
#include "freshness.h"
#include "graph_optimization.h"
#include "jobserver.h"
#include "load_limit.h"
#include "log.h"
#include "process.h"
#include "ready_queue.h"
#include "stats.h"
#include "strutil.h"
#include "symtab.h"
#include "var.h"

namespace {

constexpr FileStamp kNotExist = FileStamp::Missing();
constexpr FileStamp kIgnoredEdge = FileStamp::Ignored();

int CommandExitStatus(int status) {
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return status;
}

static FileStamp LowResolutionTimestamp(FileStamp timestamp) {
  return timestamp.LowResolution();
}

static bool IsRecursiveKatiCommandMarker(const std::string& command) {
  // Recursive Kati commands may be wrapped in shell conditionals, directory
  // setup, or environment exports before the marker. Looking only at the
  // first token made those commands consume a jobserver token while their
  // child Kati tried to acquire the remaining token, causing a recursive
  // deadlock (especially at -j1). KATI_DEPTH remains the preferred marker for
  // commands emitted by older graphs. New graphs keep MAKE as a single safe
  // executable path, so recognize that path as a recursive boundary too.
  if (command.find("KATI_DEPTH=") != std::string::npos)
    return true;

  const std::string& executable = g_flags.executable_path;
  if (executable.empty())
    return false;

  size_t pos = command.find(executable);
  while (pos != std::string::npos) {
    // Exported MAKE=/path/to/omktr is data, not a recursive invocation.
    // Recipe environment prefixes are common, and treating one as a command
    // would execute an ordinary recipe even during a recursive dry run.
    size_t value_start = pos;
    if (value_start > 0 &&
        (command[value_start - 1] == '\'' || command[value_start - 1] == '"'))
      --value_start;
    const bool assignment_value =
        value_start > 0 && command[value_start - 1] == '=';
    const bool start = pos == 0 || isspace(command[pos - 1]) ||
                       command[pos - 1] == '\'' || command[pos - 1] == '"' ||
                       command[pos - 1] == '=' || command[pos - 1] == ';';
    const size_t end = pos + executable.size();
    const bool finish = end == command.size() || isspace(command[end]) ||
                        command[end] == '\'' || command[end] == '"' ||
                        command[end] == ';';
    if (start && finish && !assignment_value)
      return true;
    pos = command.find(executable, pos + 1);
  }
  return false;
}

class Executor {
  struct Ancestry {
    Symbol output;
    std::shared_ptr<const Ancestry> parent;
  };

 public:
  explicit Executor(Evaluator* ev, bool parallel, bool runtime_bootstrap)
      : ce_(ev),
        job_limit_(EffectiveJobLimit()),
        num_commands_(0),
        parallel_(parallel && job_limit_ > 1),
        runtime_bootstrap_(runtime_bootstrap) {
    shell_ = ev->GetShell();
    shellflag_ = ev->GetShellFlag();
  }

  static int EffectiveJobLimit() {
    int jobs = g_flags.num_jobs > 0 ? g_flags.num_jobs : 1;
    // A shared jobserver, when present, is the authority across recursive
    // Kati processes.  Do not reduce nested executors based on KATI_DEPTH:
    // that old heuristic serialized recursive work and made the graph's
    // effective parallelism depend on recursion depth.  Each executor may
    // schedule up to the requested local limit; RunCommand obtains a shared
    // token before starting the actual recipe.
    return jobs;
  }

  void ExplainNode(const DepNode& n,
                   FileStamp output_ts,
                   const char* reason,
                   const std::vector<Command>* expanded = nullptr,
                   const std::vector<bool>* groups = nullptr) {
    if (!g_flags.explain)
      return;
    std::vector<Command> commands;
    if (!expanded) {
      std::lock_guard<std::mutex> lock(eval_mu_);
      commands = ce_.Eval(n, output_ts);
      expanded = &commands;
    }
    std::lock_guard<std::mutex> lock(output_mu_);
    const Loc& source = n.rule_loc.filename ? n.rule_loc : n.loc;
    printf("Target: %s\n  Source: %s:%d\n  Decision: %s\n",
           n.lexical_output.c_str(),
           source.filename ? source.filename : "<input>", source.lineno,
           reason);
    if (n.vpath_provider.IsValid())
      printf("  VPATH provider: %s\n", n.vpath_provider.c_str());
    auto print_inputs = [](const char* label, const auto& inputs) {
      printf("  %s:", label);
      for (const auto& input : inputs)
        printf(" %s", input.first.c_str());
      printf("\n");
    };
    print_inputs("Prerequisites", n.deps);
    print_inputs("Order-only prerequisites", n.order_onlys);
    print_inputs("Validations", n.validations);
    if (groups) {
      for (size_t group = 0; group < groups->size(); ++group) {
        printf("  Double-colon group %zu: %s; prerequisites:", group + 1,
               !(*groups)[group] ? "up to date"
               : !n.double_colon_group_has_prerequisites[group]
                   ? "outdated (unconditional)"
                   : "outdated");
        for (Symbol input : n.double_colon_group_inputs[group])
          printf(" %s", input.c_str());
        printf("\n");
      }
    }
    for (const auto& dep : n.deps) {
      FileStamp timestamp = FileStamp::Read(dep.second->output.c_str());
      {
        std::lock_guard<std::mutex> state_lock(state_mu_);
        const auto completed = done_.find(dep.second->output);
        if (completed != done_.end())
          timestamp = completed->second.timestamp;
      }
      if (std::find(n.low_resolution_inputs.begin(),
                    n.low_resolution_inputs.end(),
                    dep.first) != n.low_resolution_inputs.end())
        timestamp = LowResolutionTimestamp(timestamp);
      if (dep.second->is_phony || IsWhatIf(dep.second->output) ||
          timestamp > output_ts) {
        printf("  Outdated prerequisite: %s (%s)\n", dep.first.c_str(),
               dep.second->is_phony           ? "phony"
               : IsWhatIf(dep.second->output) ? "forced by -W"
                                              : "newer or would be rebuilt");
      }
    }
    if (!n.cmds.empty()) {
      const Loc& loc = n.loc;
      printf("  Recipe source: %s:%d\n",
             loc.filename ? loc.filename : "<input>", loc.lineno);
    }
    printf("  Expanded recipe%s:\n", expanded->empty() ? " (empty)" : "");
    for (const Command& command : *expanded) {
      printf("    %s\n", command.cmd.c_str());
      if (IsRecursiveKatiCommand(command.cmd))
        printf(
            "    Recursive build: child Makefile evaluated at runtime; "
            "child graph is not flattened\n");
    }
    fflush(stdout);
  }

  bool DependenciesAreCurrent(const DepNode& node,
                              FileStamp target_timestamp,
                              std::unordered_set<Symbol>* visiting) {
    if (!visiting->insert(node.output).second)
      return false;
    if (node.is_phony ||
        std::find(node.double_colon_group_has_prerequisites.begin(),
                  node.double_colon_group_has_prerequisites.end(),
                  false) != node.double_colon_group_has_prerequisites.end())
      return false;
    for (const auto& dep : node.deps) {
      const DepNode& child = *dep.second;
      if (child.is_phony || IsWhatIf(child.output))
        return false;
      const FileStamp child_timestamp = FileStamp::Read(child.output.c_str());
      if (child_timestamp == kNotExist &&
          (!child.intermediate || (child.deps.empty() && child.cmds.empty()) ||
           !DependenciesAreCurrent(child, target_timestamp, visiting)))
        return false;
      if (child_timestamp != kNotExist &&
          (child_timestamp > target_timestamp ||
           !DependenciesAreCurrent(child, child_timestamp, visiting)))
        return false;
    }
    for (const auto& dep : node.order_onlys) {
      const FileStamp order_timestamp =
          FileStamp::Read(dep.second->output.c_str());
      if (dep.second->is_phony || IsWhatIf(dep.second->output) ||
          order_timestamp == kNotExist ||
          !DependenciesAreCurrent(*dep.second, order_timestamp, visiting))
        return false;
    }
    visiting->erase(node.output);
    return true;
  }

  FileStamp ExecNode(const DepNode& n,
                     const char* needed_by,
                     std::shared_ptr<const Ancestry> parent = nullptr,
                     const DepNode* parent_node = nullptr,
                     const FileStamp* initial_timestamp = nullptr,
                     std::vector<Command>* prepared_commands = nullptr) {
    for (auto path = parent; path != nullptr; path = path->parent) {
      if (path->output == n.output) {
        WARN("Circular %s <- %s dependency dropped.",
             needed_by ? needed_by : "(null)", n.output.c_str());
        return kIgnoredEdge;
      }
    }
    auto ancestry =
        parallel_ ? nullptr
                  : std::make_shared<Ancestry>(Ancestry{n.output, parent});
    {
      std::unique_lock<std::mutex> lock(state_mu_);
      for (;;) {
        auto found = done_.find(n.output);
        if (found != done_.end()) {
          if (!(found->second.outcome == NodeOutcome::Running))
            return found->second.timestamp;
          if (found->second.owner == std::this_thread::get_id()) {
            WARN("Circular %s <- %s dependency dropped.",
                 needed_by ? needed_by : "(null)", n.output.c_str());
            return kIgnoredEdge;
          }
          // Parallel execution submits only ready nodes, each owned once.
          // Serial traversal can encounter only its own ancestry above.
          CHECK(false);
        }
        if (cancel_requested_.load(std::memory_order_relaxed)) {
          done_[n.output] =
              NodeState{NodeOutcome::Failed, kNotExist, std::thread::id()};
          n.execution_stamp = done_[n.output].timestamp;
          return kNotExist;
        }
        done_[n.output] = NodeState{NodeOutcome::Running, kIgnoredEdge,
                                    std::this_thread::get_id()};
        n.execution_stamp = done_[n.output].timestamp;
        break;
      }
    }
    // Evaluator tracing uses one process-global stack.  Parallel workers
    // cannot push/pop that stack concurrently; doing so corrupts the stack
    // and can crash while expanding pattern rules.  Recipe expansion remains
    // serialized below, and serial execution keeps the usual trace frames.
    std::unique_ptr<ScopedFrame> frame;
    if (!parallel_) {
      frame = std::make_unique<ScopedFrame>(
          ce_.evaluator()->Enter(FrameType::EXEC, n.output.c_str(), n.loc));
    }
    FileStamp output_ts = initial_timestamp ? *initial_timestamp
                                            : FileStamp::Read(n.output.c_str());
    if (output_ts == kNotExist && n.vpath_provider.IsValid())
      output_ts = FileStamp::Read(n.vpath_provider.c_str());

    // GNU make does not regenerate a missing implicit intermediate merely
    // because it cleaned that file after an earlier successful build. If an
    // existing parent is newer than every source beneath the intermediate,
    // let the parent use its own timestamp without recreating the chain.
    if (!parallel_ && n.intermediate && output_ts == kNotExist && parent_node &&
        !parent_node->is_phony) {
      const FileStamp parent_ts = FileStamp::Read(needed_by);
      std::unordered_set<Symbol> visited;
      if (parent_ts != kNotExist &&
          DependenciesAreCurrent(*parent_node, parent_ts, &visited)) {
        ExplainNode(n, output_ts,
                    "missing intermediate is not needed by current parent");
        {
          std::lock_guard<std::mutex> lock(state_mu_);
          done_.erase(n.output);
        }
        return parent_ts;
      }
    }

    LOG("ExecNode: %s for %s", n.output.c_str(),
        needed_by ? needed_by : "(null)");

    if (!n.has_rule && output_ts == kNotExist && !n.is_phony) {
      ExplainNode(n, output_ts, "missing input with no producing rule");
      // Several dependency checks can already be in flight when the first
      // fatal error requests cancellation.  Report only that first missing
      // prerequisite unless keep-going mode explicitly asks for all errors.
      const bool report_missing_target =
          g_flags.keep_going ||
          !cancel_requested_.exchange(true, std::memory_order_relaxed);
      if (report_missing_target) {
        std::lock_guard<std::mutex> lock(output_mu_);
        if (needed_by) {
          fprintf(stderr, "*** No rule to make target '%s', needed by '%s'.\n",
                  n.output.c_str(), needed_by);
        } else {
          fprintf(stderr, "*** No rule to make target '%s'.\n",
                  n.output.c_str());
        }
      }
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Failed, kNotExist, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      failed_.store(true, std::memory_order_relaxed);
      return kNotExist;
    }

    FileStamp latest = kIgnoredEdge;
    bool dependency_failed = false;
    auto is_low_resolution = [&n](Symbol input) {
      return std::find(n.low_resolution_inputs.begin(),
                       n.low_resolution_inputs.end(),
                       input) != n.low_resolution_inputs.end();
    };
    auto visit_child = [&](const auto& d, bool order_only) {
      FileStamp ts = kIgnoredEdge;
      bool ignored = false;
      if (parallel_) {
        const auto parent = ignored_inputs_.find(n.output);
        if (parent != ignored_inputs_.end()) {
          const auto input = parent->second.find(d.second->output);
          if (input != parent->second.end()) {
            ts = input->second;
            ignored = true;
          }
        }
      }
      if (!ignored) {
        ts = ExecNode(*d.second, n.output.c_str(), ancestry, &n);
        if (IsFailed(d.second->output))
          dependency_failed = true;
      }
      // Order-only prerequisites must be built before the recipe, but do
      // not participate in the timestamp comparison.  This includes
      // phony recursive submake barriers: GNU make does not rebuild a
      // library merely because such a prerequisite ran.
      if (!order_only && IsWhatIf(d.second->output))
        latest = FileStamp::Updated();
      else if (d.second->is_phony && !order_only)
        latest = FileStamp::Updated();
      else if (!order_only && latest < ts)
        latest = is_low_resolution(d.first) ? LowResolutionTimestamp(ts) : ts;
    };

    for (auto const& d : n.order_onlys)
      visit_child(d, true);
    for (auto const& d : n.deps)
      visit_child(d, false);

    if (dependency_failed) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Failed, output_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      failed_.store(true, std::memory_order_relaxed);
      return output_ts;
    }

    // Recursive Kati recipes are different: their visible output is often an
    // already-existing directory, while the recipe creates an aggregate file
    // below it. Ordinary targets can take the timestamp fast path without
    // expanding their recipes, as in the original lazy executor behavior.
    struct stat output_stat;
    const bool output_is_directory =
        stat(n.output.c_str(), &output_stat) == 0 &&
        S_ISDIR(output_stat.st_mode);

    // GNU make's old-file mode prevents the named existing target from being
    // remade, but its real timestamp still participates in comparisons for
    // dependents. Missing targets follow the normal path so this mode does
    // not invent an output file.
    if (IsOldFile(n.output) && output_ts != kNotExist && n.has_rule) {
      ExplainNode(n, output_ts, "kept unchanged by -o");
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Succeeded, output_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      return output_ts;
    }

    // Touch mode skips phony recipes. Real files are touched below, after
    // checking whether their prerequisites actually require an update.
    if (g_flags.is_touch && n.is_phony) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] = NodeState{
            dependency_failed ? NodeOutcome::Failed : NodeOutcome::Succeeded,
            output_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      return output_ts;
    }

    if (!DecideFreshness(output_ts, latest, n.is_phony).dirty() &&
        !output_is_directory && n.double_colon_group_inputs.empty()) {
      ExplainNode(n, output_ts, "up to date");
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Succeeded, output_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      return output_ts;
    }

    // Two missing timestamps compare equal, but a target with a recipe still
    // needs to run in that case. Expand recipes only after the fast path;
    // this also keeps expensive variable and shell-function evaluation out of
    // incremental no-op builds.
    auto double_colon_group_needs_build = [&](size_t group,
                                              FileStamp target_ts) {
      if (target_ts == kNotExist || n.is_phony ||
          !n.double_colon_group_has_prerequisites[group])
        return true;
      for (Symbol input : n.double_colon_group_inputs[group]) {
        if (IsWhatIf(input))
          return true;
        for (const auto& dep : n.deps) {
          if (!(dep.first == input) && !(dep.second->lexical_output == input))
            continue;
          if (dep.second->is_phony)
            return true;
          std::lock_guard<std::mutex> lock(state_mu_);
          const auto completed = done_.find(dep.second->output);
          if (completed != done_.end() &&
              completed->second.timestamp > target_ts)
            return true;
        }
        if (FileStamp::Read(input.c_str()) > target_ts)
          return true;
      }
      return false;
    };
    std::vector<bool> group_needs_build;
    for (size_t group = 0; group < n.double_colon_group_inputs.size(); ++group)
      group_needs_build.push_back(
          double_colon_group_needs_build(group, output_ts));

    if (g_flags.is_touch && !n.cmds.empty() && !output_is_directory &&
        (group_needs_build.empty() ||
         std::any_of(group_needs_build.begin(), group_needs_build.end(),
                     [](bool dirty) { return dirty; }))) {
      ScopedFilesystemEffect effect;
      if (n.vpath_provider.IsValid()) {
        n.vpath_discarded.store(true, std::memory_order_release);
        struct stat st;
        if (lstat(n.output.c_str(), &st) == 0 && S_ISLNK(st.st_mode) &&
            unlink(n.output.c_str()) != 0)
          PERROR("unlink %s", n.output.c_str());
      }
      const int fd = open(n.output.c_str(), O_WRONLY | O_CREAT, 0666);
      if (fd < 0 || close(fd) != 0 ||
          utimensat(AT_FDCWD, n.output.c_str(), nullptr, 0) < 0)
        PERROR("touch %s", n.output.c_str());
      const FileStamp touched_ts = FileStamp::Read(n.output.c_str());
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Succeeded, touched_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      return touched_ts;
    }

    // Expand only the recipes of groups that were dirty before any recipe
    // runs. Expansion itself can invoke $(shell), $(file), or $(error).
    std::vector<Command> commands;
    std::string bootstrap_owner, bootstrap_identity;
    if (prepared_commands) {
      commands = std::move(*prepared_commands);
    } else {
      std::lock_guard<std::mutex> lock(eval_mu_);
      std::unique_ptr<RuntimeRecipeReadCapture> capture;
      if (runtime_bootstrap_ && !g_flags.is_dry_run && !g_flags.is_question &&
          !g_flags.explain && !n.is_phony && !output_is_directory &&
          n.double_colon_group_inputs.empty()) {
        const std::string directory =
            g_flags.ninja_dir ? g_flags.ninja_dir : ".";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
          ERROR("cannot create bootstrap recipe directory: %s",
                directory.c_str());
        std::vector<Command> metadata;
        if (MakeRuntimeRecipe(n, ce_.evaluator(), directory, &metadata)) {
          bootstrap_identity = "OMKTR_RECIPE1\n" + n.output.str();
          bootstrap_identity += '\0';
          bootstrap_identity += metadata.front().runtime_identity;
          AbsPath(directory + "/.kati_bootstrap-" + GraphDigest(n.output.str()),
                  &bootstrap_owner);
          WriteNinjaRecipeState(bootstrap_owner, bootstrap_identity);
          unlink((bootstrap_owner + ".reads-pending").c_str());
          capture.reset(
              new RuntimeRecipeReadCapture(metadata.front(), ce_.evaluator()));
        }
      }
      if (group_needs_build.empty() || g_flags.explain) {
        commands = ce_.Eval(n, output_ts);
      } else {
        for (size_t group = 0; group < group_needs_build.size(); ++group) {
          if (!group_needs_build[group])
            continue;
          auto group_commands = ce_.Eval(n, output_ts, group);
          commands.insert(commands.end(),
                          std::make_move_iterator(group_commands.begin()),
                          std::make_move_iterator(group_commands.end()));
        }
      }
      if (capture)
        capture->Finish(bootstrap_owner);
    }
    // A directory is a normal timestamped target unless its recipe enters a
    // recursive make that may update files inside it without changing the
    // directory timestamp.  Keep that recursive case live, but do not rerun
    // ordinary directory-creation recipes such as `mkdir ../lib` on every
    // invocation.
    if (output_is_directory && n.double_colon_group_inputs.empty() &&
        output_ts != kNotExist && output_ts >= latest && !n.is_phony &&
        std::none_of(commands.begin(), commands.end(), [](const Command& c) {
          return IsRecursiveKatiCommand(c.cmd);
        })) {
      ExplainNode(n, output_ts, "up to date (directory)", &commands);
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Succeeded, output_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      return output_ts;
    }
    // GNU make considers a missing target with an explicit empty rule
    // updated for this invocation, even though no file was created.  Its
    // dependents must therefore rebuild on every invocation.
    const bool missing_empty_rule =
        n.has_rule && commands.empty() &&
        (output_ts == kNotExist ||
         (n.vpath_provider.IsValid() && !n.cmds.empty() &&
          FileStamp::Read(n.output.c_str()) == kNotExist));
    FileStamp logical_ts =
        missing_empty_rule ? FileStamp::Updated() : output_ts;

    if (n.vpath_provider.IsValid() && (n.is_phony || !n.cmds.empty())) {
      ScopedFilesystemEffect effect;
      n.vpath_discarded.store(true, std::memory_order_release);
      struct stat st;
      if (!g_flags.is_dry_run && !g_flags.is_question &&
          lstat(n.output.c_str(), &st) == 0 && S_ISLNK(st.st_mode) &&
          unlink(n.output.c_str()) != 0)
        PERROR("unlink %s", n.output.c_str());
    }

    if (g_flags.is_question) {
      const bool any_group_needs_build =
          std::any_of(group_needs_build.begin(), group_needs_build.end(),
                      [](bool needed) { return needed; });
      if (n.is_phony ||
          (!commands.empty() &&
           (n.double_colon_group_inputs.empty() || any_group_needs_build))) {
        needs_build_.store(true, std::memory_order_relaxed);
        if (g_flags.explain)
          logical_ts = FileStamp::Updated();
      }
      const char* reason =
          n.is_phony || output_ts == kNotExist
              ? DecideFreshness(output_ts, latest, n.is_phony).explanation()
          : !group_needs_build.empty()
              ? (any_group_needs_build
                     ? "outdated: double-colon group is unconditional or has "
                       "an outdated prerequisite"
                     : "up to date: all double-colon groups are current")
          : output_ts < latest
              ? DecideFreshness(output_ts, latest, n.is_phony).explanation()
              : "outdated: recursive directory recipe";
      ExplainNode(n, output_ts, reason, &commands, &group_needs_build);
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{NodeOutcome::Succeeded, logical_ts, std::thread::id()};
        n.execution_stamp = done_[n.output].timestamp;
      }
      return logical_ts;
    }

    // Record that an out-of-date target is being rebuilt.  This is also
    // needed by the included-makefile bootstrap pass: an existing included
    // file may have a rule and be rebuilt even though it was not missing.
    // The caller uses this result to decide whether GNU make-style
    // re-evaluation is required.
    if (!commands.empty() && n.double_colon_group_inputs.empty())
      needs_build_.store(true, std::memory_order_relaxed);

    bool node_failed = false;
    bool ran_recipe = false;
    const std::string output_stamp = GetOutputStamp(n.output.str());
    for (const Command& command : commands) {
      if (command.double_colon_group != static_cast<size_t>(-1))
        needs_build_.store(true, std::memory_order_relaxed);
      if (cancel_requested_.load(std::memory_order_relaxed) ||
          GetBuildSignal() != 0) {
        node_failed = true;
        break;
      }
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        num_commands_ += 1;
      }
      if (command.echo && !command.cmd.empty() && command.cmd != ":") {
        if (command.verbose)
          printf("%s\n", command.display_cmd.c_str());
        else
          printf("  BUILD   %s\n", command.output.c_str());
        fflush(stdout);
      }
      if (g_flags.is_dry_run) {
        const bool recursive = IsRecursiveKatiCommand(command.cmd);
        {
          std::lock_guard<std::mutex> lock(output_mu_);
          if (!command.cmd.empty() && command.cmd != ":")
            printf("%s\n", command.cmd.c_str());
          fflush(stdout);
        }
        // GNU make still enters recursive makes during -n/--dry-run so the
        // child can print the recipes it would execute. Ordinary recipes
        // remain unexecuted. The normalized MAKEFLAGS installed by
        // UpdateMakeFlags carries -n into the child Kati process.
        if (recursive || command.force_run) {
          auto recursive_lock = LockRecursiveCommand();
          const std::string& command_shell =
              command.shell.empty() ? shell_ : command.shell;
          const std::string& command_shellflag =
              command.shellflag.empty() ? shellflag_ : command.shellflag;

          if (cancel_requested_.load(std::memory_order_relaxed) ||
              GetBuildSignal()) {
            node_failed = true;
            break;
          }
          const int result =
              RunRecipe(command_shell, command_shellflag, command.cmd, false,
                        command.environment, command.unset_environment);
          if (result != 0 && !command.ignore_error) {
            node_failed = true;
            failed_.store(true, std::memory_order_relaxed);
            DeleteFailedOutput(n, output_ts);
            if (!g_flags.keep_going)
              cancel_requested_.store(true, std::memory_order_relaxed);
            break;
          }
        }
      } else {
        const bool recursive = IsRecursiveKatiCommand(command.cmd);
        std::unique_lock<std::mutex> recursive_lock;
        std::string command_text = command.cmd;
        if (recursive) {
          recursive_lock = LockRecursiveCommand();
          // Recursive children share the wrapper's FIFO. RunCommand passes
          // an inherited reserved slot to the child when one is available;
          // otherwise this boundary takes no FIFO token and the child takes
          // tokens for its own visible work.
          // Normal Kati execution has already installed the evaluator's
          // current exported variables in its process environment.  Do not
          // reload a sibling Ninja env.sh here: that file is the deferred
          // graph-generation snapshot and can contain an earlier value for a
          // variable later assigned by the current makefile (for example
          // srcroot in recursive Kbuild).  Ninja recipes source env.sh at
          // their own process boundary; recursive Kati must inherit this
          // process environment unchanged.
        }
        const std::string& command_shell =
            command.shell.empty() ? shell_ : command.shell;
        const std::string& command_shellflag =
            command.shellflag.empty() ? shellflag_ : command.shellflag;

        if (cancel_requested_.load(std::memory_order_relaxed) ||
            GetBuildSignal()) {
          node_failed = true;
          break;
        }
        int result = RunRecipe(command_shell, command_shellflag, command_text,
                               !recursive, command.environment,
                               command.unset_environment);
        ran_recipe = true;
        const bool interrupted = GetBuildSignal() != 0 || WIFSIGNALED(result);
        if (interrupted && !n.precious)
          DeleteChangedOutput(n.output.str(), output_stamp);
        if (result != 0 || interrupted) {
          if (command.ignore_error && !interrupted) {
            fprintf(stderr, "[%s] Error %d (ignored)\n", command.output.c_str(),
                    CommandExitStatus(result));
          } else {
            fprintf(stderr, "*** [%s] Error %d\n", command.output.c_str(),
                    CommandExitStatus(result));
            fprintf(stderr, "    command: %s\n", command.cmd.c_str());
            node_failed = true;
            failed_.store(true, std::memory_order_relaxed);
            DeleteFailedOutput(n, output_ts);
            if (!g_flags.keep_going)
              cancel_requested_.store(true, std::memory_order_relaxed);
            break;
          }
        }
      }
    }

    if (ran_recipe && !node_failed && !bootstrap_owner.empty()) {
      CommitRuntimeRecipeReads(bootstrap_owner, bootstrap_identity);
      WriteNinjaRecipeState(bootstrap_owner + ".success", bootstrap_identity);
    }
    FileStamp completed_ts =
        ran_recipe ? FileStamp::Read(n.output.c_str()) : logical_ts;
    // A successful recipe counts as updating its target for this invocation,
    // even if it deliberately leaves no file behind (Automake's force rules
    // are a common example).  Dependents must observe that logical update.
    if (ran_recipe && !node_failed && completed_ts == kNotExist)
      completed_ts = FileStamp::Updated();
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      if (ran_recipe && !node_failed)
        built_outputs_.insert(n.output);
      done_[n.output] =
          NodeState{node_failed ? NodeOutcome::Failed : NodeOutcome::Succeeded,
                    completed_ts, std::thread::id()};
      n.execution_stamp = done_[n.output].timestamp;
    }
    return completed_ts;
  }

  uint64_t Count() { return num_commands_; }

  int RunRecipe(const std::string& shell,
                const std::string& shellflag,
                const std::string& command,
                bool acquire_job_token,
                const std::vector<std::string>& environment,
                const std::vector<std::string>& unset_environment) {
    try {
      std::string unused;
      if (g_flags.output_sync == Flags::OutputSync::kNone) {
        return RunCommand(
            shell, shellflag, command, RedirectStderr::NONE, &unused,
            acquire_job_token,
            [this](std::string_view data) {
              std::lock_guard<std::mutex> lock(output_mu_);
              fwrite(data.data(), 1, data.size(), stdout);
              fflush(stdout);
            },
            g_flags.make_level + 1, &environment, &unset_environment);
      }
      if (g_flags.output_sync == Flags::OutputSync::kLine) {
        std::string pending;
        auto emit = [this](std::string_view data) {
          std::lock_guard<std::mutex> lock(output_mu_);
          fwrite(data.data(), 1, data.size(), stdout);
          fflush(stdout);
        };
        const int status = RunCommand(
            shell, shellflag, command, RedirectStderr::STDOUT, &unused,
            acquire_job_token,
            [&](std::string_view data) {
              pending.append(data);
              size_t end;
              while ((end = pending.find('\n')) != std::string::npos) {
                emit(std::string_view(pending).substr(0, end + 1));
                pending.erase(0, end + 1);
              }
              if (pending.size() > 65536) {
                emit(pending);
                pending.clear();
              }
            },
            g_flags.make_level + 1, &environment, &unset_environment);
        if (!pending.empty())
          emit(pending);
        return status;
      }
      constexpr size_t kMemoryLimit = 1024 * 1024;
      std::unique_ptr<FILE, decltype(&fclose)> spool(nullptr, fclose);
      std::string buffered;
      const int status = RunCommand(
          shell, shellflag, command, RedirectStderr::STDOUT, &unused,
          acquire_job_token,
          [&](std::string_view data) {
            if (spool == nullptr &&
                buffered.size() + data.size() <= kMemoryLimit) {
              buffered.append(data);
              return;
            }
            if (spool == nullptr) {
              spool.reset(tmpfile());
              if (spool == nullptr)
                throw std::system_error(errno ? errno : EIO,
                                        std::generic_category(),
                                        "tmpfile failed");
              if (fwrite(buffered.data(), 1, buffered.size(), spool.get()) !=
                  buffered.size())
                throw std::system_error(errno ? errno : EIO,
                                        std::generic_category(),
                                        "spool write failed");
              buffered.clear();
            }
            if (fwrite(data.data(), 1, data.size(), spool.get()) != data.size())
              throw std::system_error(errno ? errno : EIO,
                                      std::generic_category(),
                                      "spool write failed");
          },
          g_flags.make_level + 1, &environment, &unset_environment);
      // A failed output callback has already stopped and reaped the process.
      // Preserve that status so interruption cleanup removes partial outputs.
      if (status != 0 && spool && ferror(spool.get()))
        return status;
      if (spool != nullptr && fseek(spool.get(), 0, SEEK_SET) != 0)
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "spool rewind failed");
      {
        std::lock_guard<std::mutex> lock(output_mu_);
        if (spool != nullptr) {
          char buf[8192];
          size_t count;
          while ((count = fread(buf, 1, sizeof(buf), spool.get())) != 0)
            fwrite(buf, 1, count, stdout);
        } else {
          fwrite(buffered.data(), 1, buffered.size(), stdout);
        }
        fflush(stdout);
      }
      if (spool && ferror(spool.get()))
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "spool read failed");
      return status;
    } catch (const std::system_error& error) {
      WARN("recipe output failure: %s", error.what());
      return 126 << 8;
    }
  }

  bool NeedsBuild() const {
    return needs_build_.load(std::memory_order_relaxed);
  }

  bool Failed() const {
    return failed_.load(std::memory_order_relaxed) || GetBuildSignal() != 0;
  }

  bool IsWhatIf(Symbol output) const {
    const std::string name(TrimLeadingCurdir(output.str()));
    for (const std::string& file : g_flags.what_if_files) {
      if (file == name)
        return true;
    }
    return false;
  }

  bool IsOldFile(Symbol output) const {
    const std::string name(TrimLeadingCurdir(output.str()));
    for (const std::string& file : g_flags.old_files) {
      if (file == name)
        return true;
    }
    return false;
  }

  void RunRoots(const std::vector<NamedDepNode>& roots) {
    if (!parallel_) {
      for (const auto& root : roots)
        ExecNode(*root.second, nullptr);
      return;
    }
    RunReadyGraph(roots);
  }

  bool WasBuilt(Symbol output) {
    std::lock_guard<std::mutex> lock(state_mu_);
    return built_outputs_.count(output) != 0;
  }

 private:
  bool CanPrepareInlineCommands(const DepNode& node) {
    // Kbuild-style phony ':' recipes still need normal expansion (including
    // exported values and target-specific shells). Prepare them once after
    // successful prerequisites, then dispatch only if they require a shell.
    if (runtime_bootstrap_ || !node.is_phony || node.cmds.size() != 1 ||
        !node.cmds.front()->IsLiteral() ||
        !node.double_colon_group_inputs.empty() || g_flags.is_touch ||
        g_flags.is_question ||
        cancel_requested_.load(std::memory_order_relaxed) || GetBuildSignal())
      return false;
    std::string_view literal =
        TrimLeftSpace(node.cmds.front()->GetLiteralValueUnsafe());
    while (!literal.empty() &&
           (literal.front() == '@' || literal.front() == '-' ||
            literal.front() == '+')) {
      literal.remove_prefix(1);
      literal = TrimLeftSpace(literal);
    }
    if (literal != ":")
      return false;
    std::lock_guard<std::mutex> lock(state_mu_);
    for (const auto* edges : {&node.deps, &node.order_onlys})
      for (const auto& edge : *edges) {
        const auto input = done_.find(edge.second->output);
        if (input == done_.end() ||
            (input->second.outcome == NodeOutcome::Running) ||
            (input->second.outcome == NodeOutcome::Failed))
          return false;
      }
    return true;
  }

  bool CanFinishInline(const DepNode& node, FileStamp timestamp) {
    if (cancel_requested_.load(std::memory_order_relaxed) || GetBuildSignal())
      return true;
    if (timestamp == kNotExist || node.is_phony ||
        !node.double_colon_group_inputs.empty() || FilesystemEffectsActive())
      return false;
    std::lock_guard<std::mutex> lock(state_mu_);
    for (const auto& input : node.deps) {
      if (input.second->is_phony || IsWhatIf(input.second->output))
        return false;
      auto completed = done_.find(input.second->output);
      if (completed == done_.end() ||
          (completed->second.outcome == NodeOutcome::Running))
        return false;
      FileStamp input_time = completed->second.timestamp;
      if (std::find(node.low_resolution_inputs.begin(),
                    node.low_resolution_inputs.end(),
                    input.first) != node.low_resolution_inputs.end())
        input_time = LowResolutionTimestamp(input_time);
      if (input_time > timestamp)
        return false;
    }
    struct stat metadata;
    return stat(node.output.c_str(), &metadata) == 0 &&
           !S_ISDIR(metadata.st_mode);
  }

  void RunReadyGraph(const std::vector<NamedDepNode>& roots) {
    std::vector<const DepNode*> nodes;
    std::vector<const char*> needed_by;
    std::vector<FileStamp> timestamps;
    std::vector<ReadyTask> tasks;
    std::unordered_map<Symbol, size_t> ids;
    std::vector<unsigned char> colors;
    auto add = [&](const DepNode* node, const char* parent) {
      const size_t id = nodes.size();
      ids.emplace(node->output, id);
      nodes.push_back(node);
      needed_by.push_back(parent);
      tasks.emplace_back();
      tasks.back().inline_work = node->cmds.empty();
      colors.push_back(1);
      FileStamp timestamp = FileStamp::Read(node->output.c_str());
      if (timestamp == kNotExist && node->vpath_provider.IsValid())
        timestamp = FileStamp::Read(node->vpath_provider.c_str());
      timestamps.push_back(timestamp);
      return id;
    };
    {
      COLLECT_STATS("executor ready graph");
      for (const auto& root : roots) {
        if (ids.count(root.second->output))
          continue;
        std::vector<std::pair<size_t, size_t>> stack{
            {add(root.second, nullptr), 0}};
        while (!stack.empty()) {
          if (GetBuildSignal())
            return;
          const size_t id = stack.back().first;
          const DepNode& node = *nodes[id];
          const size_t edge = stack.back().second++;
          if (edge == node.order_onlys.size() + node.deps.size()) {
            colors[id] = 2;
            auto& inputs = tasks[id].dependencies;
            std::sort(inputs.begin(), inputs.end());
            inputs.erase(std::unique(inputs.begin(), inputs.end()),
                         inputs.end());
            stack.pop_back();
            continue;
          }
          const DepNode* child =
              edge < node.order_onlys.size()
                  ? node.order_onlys[edge].second
                  : node.deps[edge - node.order_onlys.size()].second;
          // A missing implicit intermediate can be unnecessary for one parent
          // but required by another. Store that decision on the edge, never as
          // a globally completed node.
          const FileStamp parent_time = timestamps[id];
          if (child->intermediate && !node.is_phony &&
              parent_time != kNotExist &&
              FileStamp::Read(child->output.c_str()) == kNotExist) {
            std::unordered_set<Symbol> visiting;
            if (DependenciesAreCurrent(node, parent_time, &visiting)) {
              ignored_inputs_[node.output][child->output] = parent_time;
              continue;
            }
          }
          const auto found = ids.find(child->output);
          if (found != ids.end() && colors[found->second] == 1) {
            WARN("Circular %s <- %s dependency dropped.", node.output.c_str(),
                 child->output.c_str());
            ignored_inputs_[node.output][child->output] = kIgnoredEdge;
            continue;
          }
          const bool discovered = found == ids.end();
          const size_t child_id =
              discovered ? add(child, node.output.c_str()) : found->second;
          tasks[id].dependencies.push_back(child_id);
          if (discovered)
            stack.emplace_back(child_id, 0);
        }
      }
    }
    RecordStatCounter("scheduler_nodes", nodes.size());
    std::vector<std::unique_ptr<std::vector<Command>>> prepared(nodes.size());
    const ReadyQueueStats stats = RunReadyQueue(
        // One preparation slot overlaps recipe expansion/output bookkeeping
        // with the admitted recipes. ProcessLease remains the authority for the
        // local recipe limit; preparation does not increase that limit.
        tasks, static_cast<size_t>(job_limit_) + 1,
        [&](size_t id) {
          ExecNode(*nodes[id], needed_by[id], nullptr, nullptr, &timestamps[id],
                   prepared[id] ? &*prepared[id] : nullptr);
        },
        [&](size_t id) {
          if (CanFinishInline(*nodes[id], timestamps[id]))
            return true;
          if (!prepared[id] && CanPrepareInlineCommands(*nodes[id])) {
            std::lock_guard<std::mutex> lock(eval_mu_);
            prepared[id] = std::make_unique<std::vector<Command>>(
                ce_.Eval(*nodes[id], timestamps[id]));
          }
          return prepared[id] &&
                 std::all_of(prepared[id]->begin(), prepared[id]->end(),
                             [&](const Command& command) {
                               return command.cmd == ":" &&
                                      (command.shell.empty()
                                           ? shell_
                                           : command.shell) == "/bin/sh" &&
                                      (command.shellflag.empty()
                                           ? shellflag_
                                           : command.shellflag) == "-c";
                             });
        },
        [&] {
          cancel_requested_.store(true, std::memory_order_relaxed);
          CancelBuildProcesses();
        });
    RecordStatCounter("scheduler_workers", stats.workers);
    RecordStatCounter("scheduler_inline_tasks", stats.inline_tasks);
    RecordStatMaximum("scheduler_peak_active", stats.peak_active);
  }

  enum class NodeOutcome { Running, Succeeded, Failed };
  struct NodeState {
    NodeOutcome outcome;
    FileStamp timestamp;
    std::thread::id owner;
  };

  bool IsFailed(Symbol output) {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto it = done_.find(output);
    return it != done_.end() && (it->second.outcome == NodeOutcome::Failed);
  }

  std::unique_lock<std::mutex> LockRecursiveCommand() {
    std::unique_lock<std::mutex> lock(recursive_command_mu_, std::defer_lock);
    // A shared FIFO already limits the visible work across child executors.
    // Serializing independent recursive recipes as well prevents their small
    // subgraphs from filling that budget (notably Kbuild subdirectories).
    // Without a shared jobserver retain the conservative fallback: each
    // child otherwise has its own full local -j limit.
    if (!IsKatiJobserverAvailable())
      lock.lock();
    return lock;
  }

  void DeleteFailedOutput(const DepNode& node, FileStamp previous_ts) {
    if (node.delete_on_error && !node.precious &&
        FileStamp::Read(node.output.str()) != previous_ts) {
      ScopedFilesystemEffect effect;
      unlink(node.output.c_str());
    }
  }

  CommandEvaluator ce_;
  std::unordered_map<Symbol, NodeState> done_;
  std::unordered_map<Symbol, std::unordered_map<Symbol, FileStamp>>
      ignored_inputs_;
  std::unordered_set<Symbol> built_outputs_;
  std::mutex state_mu_;
  std::mutex eval_mu_;
  std::mutex output_mu_;
  std::mutex recursive_command_mu_;
  int job_limit_;
  std::string shell_;
  std::string shellflag_;
  uint64_t num_commands_;
  std::atomic<bool> needs_build_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> cancel_requested_{false};
  bool parallel_;
  bool runtime_bootstrap_;
};

}  // namespace

bool IsRecursiveKatiCommand(const std::string& command) {
  return IsRecursiveKatiCommandMarker(command);
}

ExecResult Exec(const std::vector<NamedDepNode>& roots,
                Evaluator* ev,
                bool parallel,
                bool runtime_bootstrap,
                bool announce_noop) {
  if (g_flags.explain)
    parallel = false;
  ScopedBuildSignals signals;
  for (const auto& root : roots) {
    if (root.second->is_notparallel) {
      parallel = false;
      break;
    }
  }
  Executor executor(ev, parallel, runtime_bootstrap);
  executor.RunRoots(roots);
  std::unordered_set<Symbol> visited;
  std::vector<DepNode*> pending;
  for (const auto& root : roots)
    pending.push_back(root.second);
  while (!pending.empty()) {
    DepNode* node = pending.back();
    pending.pop_back();
    if (!visited.insert(node->output).second)
      continue;
    if (node->intermediate && !g_flags.is_dry_run && !g_flags.is_question &&
        executor.WasBuilt(node->output)) {
      ScopedFilesystemEffect effect;
      unlink(node->output.str().c_str());
    }
    for (const auto& dep : node->deps)
      pending.push_back(dep.second);
    for (const auto& dep : node->order_onlys)
      pending.push_back(dep.second);
    for (const auto& dep : node->validations)
      pending.push_back(dep.second);
  }
  const char* kati_verbose = getenv("KATI_VERBOSE");
  if (announce_noop && executor.Count() == 0 && !g_flags.explain &&
      !g_flags.is_silent_mode &&
      !(kati_verbose != nullptr && std::string_view(kati_verbose) == "1")) {
    for (auto const& root : roots) {
      printf("kati: Nothing to be done for `%s'.\n", root.first.c_str());
    }
  }
  bool roots_built = false;
  for (const auto& root : roots) {
    if (executor.WasBuilt(root.second->output)) {
      roots_built = true;
      break;
    }
  }
  return ExecResult{executor.NeedsBuild(), executor.Failed(), roots_built};
}
