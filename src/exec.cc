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
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "command.h"
#include "dep.h"
#include "eval.h"
#include "expr.h"
#include "fileutil.h"
#include "flags.h"
#include "log.h"
#include "strutil.h"
#include "symtab.h"
#include "var.h"

namespace {

const double kNotExist = -2.0;
const double kProcessing = -1.0;

int CommandExitStatus(int status) {
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return status;
}

static double LowResolutionTimestamp(double timestamp) {
  return std::floor(timestamp);
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
    // Exported MAKE=/path/to/ckati is data, not a recursive invocation.
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

class WorkerPool {
 public:
  struct Task {
    explicit Task(std::function<double()> f)
        : function(std::move(f)), result(0), complete(false) {}
    std::function<double()> function;
    double result;
    bool complete;
  };
  using TaskHandle = std::shared_ptr<Task>;

  explicit WorkerPool(int worker_count)
      : max_workers_(worker_count), stopping_(false) {
    workers_.reserve(worker_count);
  }

  ~WorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_)
      worker.join();
  }

  TaskHandle Submit(std::function<double()> function) {
    TaskHandle task = std::make_shared<Task>(std::move(function));
    {
      std::lock_guard<std::mutex> lock(mu_);
      CHECK(!stopping_);
      tasks_.emplace_back(task);
      queued_[task.get()] = std::prev(tasks_.end());
      // Keep the pool persistent, but avoid creating a full -jN thread set in
      // every recursive Kati process before that process has work for it.
      if (static_cast<int>(workers_.size()) < max_workers_)
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
    cv_.notify_one();
    return task;
  }

  // A worker waiting for a dependency helps drain queued work. Without this,
  // all pool workers could wait on tasks queued behind them.
  double Wait(const TaskHandle& target) {
    while (true) {
      TaskHandle task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        if (target->complete)
          return target->result;
        auto queued = queued_.find(target.get());
        if (queued == queued_.end()) {
          cv_.wait(lock, [&]() {
            return target->complete || queued_.count(target.get()) != 0;
          });
          continue;
        }
        task = std::move(*queued->second);
        tasks_.erase(queued->second);
        queued_.erase(queued);
      }
      RunTask(task);
    }
  }

 private:
  void RunTask(const TaskHandle& task) {
    const double result = task->function();
    {
      std::lock_guard<std::mutex> lock(mu_);
      task->result = result;
      task->complete = true;
    }
    cv_.notify_all();
  }

  void WorkerLoop() {
    while (true) {
      TaskHandle task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty())
          return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
        queued_.erase(task.get());
      }
      RunTask(task);
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::list<TaskHandle> tasks_;
  std::unordered_map<Task*, std::list<TaskHandle>::iterator> queued_;
  std::vector<std::thread> workers_;
  int max_workers_;
  bool stopping_;
};

class Executor {
  struct Ancestry {
    Symbol output;
    std::shared_ptr<const Ancestry> parent;
  };

 public:
  explicit Executor(Evaluator* ev, bool parallel)
      : ce_(ev),
        job_limit_(EffectiveJobLimit()),
        active_jobs_(0),
        num_commands_(0),
        parallel_(parallel),
        worker_pool_(parallel ? job_limit_ : 0) {
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

  bool DependenciesAreCurrent(const DepNode& node,
                              double target_timestamp,
                              std::unordered_set<Symbol>* visiting) {
    if (!visiting->insert(node.output).second)
      return false;
    for (const auto& dep : node.deps) {
      const DepNode& child = *dep.second;
      if (child.is_phony || IsWhatIf(child.output))
        return false;
      const double child_timestamp = GetTimestamp(child.output.c_str());
      if (child_timestamp == kNotExist &&
          (!child.intermediate || child.deps.empty() ||
           !DependenciesAreCurrent(child, target_timestamp, visiting)))
        return false;
      if (child_timestamp != kNotExist &&
          (child_timestamp > target_timestamp ||
           !DependenciesAreCurrent(child, child_timestamp, visiting)))
        return false;
    }
    for (const auto& dep : node.order_onlys) {
      const double order_timestamp = GetTimestamp(dep.second->output.c_str());
      if (dep.second->is_phony || IsWhatIf(dep.second->output) ||
          order_timestamp == kNotExist ||
          !DependenciesAreCurrent(*dep.second, order_timestamp, visiting))
        return false;
    }
    visiting->erase(node.output);
    return true;
  }

  double ExecNode(const DepNode& n,
                  const char* needed_by,
                  std::shared_ptr<const Ancestry> parent = nullptr,
                  const DepNode* parent_node = nullptr) {
    for (auto path = parent; path != nullptr; path = path->parent) {
      if (path->output == n.output) {
        WARN("Circular %s <- %s dependency dropped.",
             needed_by ? needed_by : "(null)", n.output.c_str());
        return kProcessing;
      }
    }
    auto ancestry = std::make_shared<Ancestry>(Ancestry{n.output, parent});
    {
      std::unique_lock<std::mutex> lock(state_mu_);
      auto found = done_.find(n.output);
      if (found != done_.end()) {
        if (!found->second.processing)
          return found->second.timestamp;
        if (found->second.owner == std::this_thread::get_id()) {
          WARN("Circular %s <- %s dependency dropped.",
               needed_by ? needed_by : "(null)", n.output.c_str());
          return kProcessing;
        }
        // A parallel dependency cycle can span workers: thread A may wait
        // for a node owned by thread B while B waits for a node owned by A.
        // The old check handled only the same-thread form, leaving both
        // workers asleep forever.  Follow the current wait-for chain while
        // holding state_mu_ and drop the edge if it returns to this thread.
        const std::thread::id current_thread = std::this_thread::get_id();
        std::thread::id owner = found->second.owner;
        bool cross_thread_cycle = false;
        while (owner != current_thread) {
          auto waiting = waiting_on_.find(owner);
          if (waiting == waiting_on_.end())
            break;
          auto dependency = done_.find(waiting->second);
          if (dependency == done_.end() || !dependency->second.processing)
            break;
          owner = dependency->second.owner;
        }
        if (owner == current_thread)
          cross_thread_cycle = true;
        if (cross_thread_cycle) {
          WARN("Circular %s <- %s dependency dropped.",
               needed_by ? needed_by : "(null)", n.output.c_str());
          return kProcessing;
        }
        waiting_on_[current_thread] = n.output;
        state_cv_.wait(lock, [&] {
          auto current = done_.find(n.output);
          return current == done_.end() || !current->second.processing;
        });
        waiting_on_.erase(current_thread);
        auto current = done_.find(n.output);
        return current == done_.end() ? kNotExist : current->second.timestamp;
      }
      if (cancel_requested_.load(std::memory_order_relaxed)) {
        done_[n.output] = NodeState{false, kNotExist, true, std::thread::id()};
        return kNotExist;
      }
      done_[n.output] =
          NodeState{true, kProcessing, false, std::this_thread::get_id()};
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
    double output_ts = GetTimestamp(n.output.c_str());

    // GNU make does not regenerate a missing implicit intermediate merely
    // because it cleaned that file after an earlier successful build. If an
    // existing parent is newer than every source beneath the intermediate,
    // let the parent use its own timestamp without recreating the chain.
    if (n.intermediate && output_ts == kNotExist && parent_node) {
      const double parent_ts = GetTimestamp(needed_by);
      std::unordered_set<Symbol> visited;
      if (parent_ts != kNotExist &&
          DependenciesAreCurrent(*parent_node, parent_ts, &visited)) {
        {
          std::lock_guard<std::mutex> lock(state_mu_);
          done_.erase(n.output);
        }
        state_cv_.notify_all();
        return parent_ts;
      }
    }

    LOG("ExecNode: %s for %s", n.output.c_str(),
        needed_by ? needed_by : "(null)");

    if (!n.has_rule && output_ts == kNotExist && !n.is_phony) {
      {
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
        done_[n.output] = NodeState{false, kNotExist, true, std::thread::id()};
      }
      failed_.store(true, std::memory_order_relaxed);
      if (!g_flags.keep_going)
        cancel_requested_.store(true, std::memory_order_relaxed);
      state_cv_.notify_all();
      return kNotExist;
    }

    double latest = kProcessing;
    std::vector<std::tuple<WorkerPool::TaskHandle, bool, bool, bool, Symbol>>
        children;
    bool dependency_failed = false;
    auto is_low_resolution = [&n](Symbol input) {
      return std::find(n.low_resolution_inputs.begin(),
                       n.low_resolution_inputs.end(),
                       input) != n.low_resolution_inputs.end();
    };
    auto visit_child = [&](const auto& d, bool order_only) {
      // A dependency subgraph is independent from its siblings.  Dispatch
      // the whole subgraph asynchronously and wait for all futures before
      // running this node's recipe.  Limiting this to leaves serialized
      // recursive/aggregate builds and made Ninja -jN effectively single
      // threaded at the top of large graphs.
      // Worker tasks expand descendants inline.  Otherwise every worker can
      // wait for a task owned by another waiting worker, starving the finite
      // pool even though the dependency graph itself is acyclic.
      if (parallel_ && n.deps.size() + n.order_onlys.size() > 1) {
        children.emplace_back(
            worker_pool_.Submit(
                [this, child = d.second, output = n.output, ancestry, &n]() {
                  return ExecNode(*child, output.c_str(), ancestry, &n);
                }),
            d.second->is_phony, order_only, is_low_resolution(d.first),
            d.second->output);
      } else {
        double ts = ExecNode(*d.second, n.output.c_str(), ancestry, &n);
        if (IsFailed(d.second->output))
          dependency_failed = true;
        // Order-only prerequisites must be built before the recipe, but do
        // not participate in the timestamp comparison.  This includes
        // phony recursive submake barriers: GNU make does not rebuild a
        // library merely because such a prerequisite ran.
        if (!order_only && IsWhatIf(d.second->output))
          latest = std::numeric_limits<double>::infinity();
        else if (d.second->is_phony && !order_only)
          latest = std::numeric_limits<double>::infinity();
        else if (!order_only && latest < ts)
          latest = is_low_resolution(d.first) ? LowResolutionTimestamp(ts) : ts;
      }
    };

    for (auto const& d : n.order_onlys)
      visit_child(d, true);
    for (auto const& d : n.deps)
      visit_child(d, false);

    for (auto& child : children) {
      double ts = worker_pool_.Wait(std::get<0>(child));
      dependency_failed = dependency_failed || IsFailed(std::get<4>(child));
      if (!std::get<2>(child) && IsWhatIf(std::get<4>(child)))
        latest = std::numeric_limits<double>::infinity();
      else if (std::get<1>(child) && !std::get<2>(child))
        latest = std::numeric_limits<double>::infinity();
      else if (!std::get<2>(child) && latest < ts)
        latest = std::get<3>(child) ? LowResolutionTimestamp(ts) : ts;
    }

    if (dependency_failed) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] = NodeState{false, output_ts, true, std::thread::id()};
      }
      failed_.store(true, std::memory_order_relaxed);
      state_cv_.notify_all();
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
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] = NodeState{false, output_ts, false, std::thread::id()};
      }
      state_cv_.notify_all();
      return output_ts;
    }

    // GNU make's touch mode does not run recipes for phony targets, and
    // updates existing rule outputs without running their recipes. Missing
    // non-phony outputs still follow the normal build path so touch mode
    // cannot fabricate a target that has no file yet.
    if (g_flags.is_touch && n.is_phony) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{false, output_ts, dependency_failed, std::thread::id()};
      }
      state_cv_.notify_all();
      return output_ts;
    }

    if (g_flags.is_touch && output_ts != kNotExist && n.has_rule &&
        !output_is_directory) {
      if (utimensat(AT_FDCWD, n.output.c_str(), nullptr, 0) < 0)
        PERROR("touch %s", n.output.c_str());
      const double touched_ts = GetTimestamp(n.output.c_str());
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{false, touched_ts, false, std::thread::id()};
      }
      state_cv_.notify_all();
      return touched_ts;
    }

    if (output_ts != kNotExist && output_ts >= latest && !n.is_phony &&
        !output_is_directory && n.double_colon_group_inputs.empty()) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] = NodeState{false, output_ts, false, std::thread::id()};
      }
      state_cv_.notify_all();
      return output_ts;
    }

    // Two missing timestamps compare equal, but a target with a recipe still
    // needs to run in that case. Expand recipes only after the fast path;
    // this also keeps expensive variable and shell-function evaluation out of
    // incremental no-op builds.
    auto double_colon_group_needs_build = [&](size_t group, double target_ts) {
      if (target_ts == kNotExist || n.is_phony ||
          n.double_colon_group_inputs[group].empty())
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
        if (GetTimestamp(input.c_str()) > target_ts)
          return true;
      }
      return false;
    };
    std::vector<bool> group_needs_build;
    for (size_t group = 0; group < n.double_colon_group_inputs.size(); ++group)
      group_needs_build.push_back(
          double_colon_group_needs_build(group, output_ts));

    // Expand only the recipes of groups that were dirty before any recipe
    // runs. Expansion itself can invoke $(shell), $(file), or $(error).
    std::vector<Command> commands;
    {
      std::lock_guard<std::mutex> lock(eval_mu_);
      if (group_needs_build.empty()) {
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
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] = NodeState{false, output_ts, false, std::thread::id()};
      }
      state_cv_.notify_all();
      return output_ts;
    }
    // GNU make considers a missing target with an explicit empty rule
    // updated for this invocation, even though no file was created.  Its
    // dependents must therefore rebuild on every invocation.
    const bool missing_empty_rule =
        n.has_rule && output_ts == kNotExist && commands.empty();
    const double logical_ts = missing_empty_rule
                                  ? std::numeric_limits<double>::infinity()
                                  : output_ts;

    if (g_flags.is_question) {
      const bool any_group_needs_build =
          std::any_of(group_needs_build.begin(), group_needs_build.end(),
                      [](bool needed) { return needed; });
      if (n.is_phony ||
          (!commands.empty() &&
           (n.double_colon_group_inputs.empty() || any_group_needs_build)))
        needs_build_.store(true, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        done_[n.output] =
            NodeState{false, logical_ts, false, std::thread::id()};
      }
      state_cv_.notify_all();
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
    for (const Command& command : commands) {
      if (command.double_colon_group != static_cast<size_t>(-1))
        needs_build_.store(true, std::memory_order_relaxed);
      if (cancel_requested_.load(std::memory_order_relaxed)) {
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
          // A recursive make is an opaque operation from this executor's
          // point of view.  Its child may update the make state, included
          // files, or directory layout that another recursive child is
          // about to inspect.  GNU make keeps the recursive invocation as a
          // single job-server job; do the equivalent here while retaining
          // parallelism inside the child executor.
          std::unique_lock<std::mutex> recursive_lock(recursive_command_mu_);
          const std::string& command_shell =
              command.shell.empty() ? shell_ : command.shell;
          const std::string& command_shellflag =
              command.shellflag.empty() ? shellflag_ : command.shellflag;
          AcquireJob();
          const int result =
              RunRecipe(command_shell, command_shellflag, command.cmd, false);
          ReleaseJob();
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
          // Recursive make commands are opaque to the current graph.  Do not
          // let sibling opaque commands mutate the same make directory at
          // the same time.  Normal compile/link recipes remain parallel, and
          // the child make keeps its own parallel executor.
          recursive_lock = std::unique_lock<std::mutex>(recursive_command_mu_);
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
        AcquireJob();
        int result = RunRecipe(command_shell, command_shellflag, command_text,
                               !recursive);
        ReleaseJob();
        ran_recipe = true;
        if (result != 0) {
          if (command.ignore_error) {
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

    double completed_ts =
        ran_recipe ? GetTimestamp(n.output.c_str()) : logical_ts;
    // A successful recipe counts as updating its target for this invocation,
    // even if it deliberately leaves no file behind (Automake's force rules
    // are a common example).  Dependents must observe that logical update.
    if (ran_recipe && !node_failed && completed_ts == kNotExist)
      completed_ts = std::numeric_limits<double>::infinity();
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      if (ran_recipe && !node_failed)
        built_outputs_.insert(n.output);
      done_[n.output] =
          NodeState{false, completed_ts, node_failed, std::thread::id()};
    }
    state_cv_.notify_all();
    return completed_ts;
  }

  uint64_t Count() { return num_commands_; }

  int RunRecipe(const std::string& shell,
                const std::string& shellflag,
                const std::string& command,
                bool acquire_job_token) {
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
          g_flags.make_level + 1);
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
          g_flags.make_level + 1);
      if (!pending.empty())
        emit(pending);
      return status;
    }
    constexpr size_t kMemoryLimit = 1024 * 1024;
    FILE* spool = nullptr;
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
            spool = tmpfile();
            if (spool == nullptr)
              PERROR("tmpfile failed");
            if (fwrite(buffered.data(), 1, buffered.size(), spool) !=
                buffered.size())
              PERROR("spool write failed");
            buffered.clear();
          }
          if (fwrite(data.data(), 1, data.size(), spool) != data.size())
            PERROR("spool write failed");
        },
        g_flags.make_level + 1);
    if (spool != nullptr && fseek(spool, 0, SEEK_SET) != 0)
      PERROR("spool rewind failed");
    {
      std::lock_guard<std::mutex> lock(output_mu_);
      if (spool != nullptr) {
        char buf[8192];
        size_t count;
        while ((count = fread(buf, 1, sizeof(buf), spool)) != 0)
          fwrite(buf, 1, count, stdout);
      } else {
        fwrite(buffered.data(), 1, buffered.size(), stdout);
      }
      fflush(stdout);
    }
    if (spool != nullptr && fclose(spool) != 0)
      PERROR("spool close failed");
    return status;
  }

  bool NeedsBuild() const {
    return needs_build_.load(std::memory_order_relaxed);
  }

  bool Failed() const { return failed_.load(std::memory_order_relaxed); }

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
    // A single aggregate root already fans out through ExecNode(). Root-level
    // dispatch is useful only when the caller requested several independent
    // goals; keeping the single-root path unchanged avoids consuming one
    // worker slot before its dependency graph is expanded.
    if (!parallel_ || roots.size() < 2) {
      for (const auto& root : roots)
        ExecNode(*root.second, nullptr);
      return;
    }

    std::vector<WorkerPool::TaskHandle> futures;
    for (const auto& root : roots) {
      futures.emplace_back(worker_pool_.Submit(
          [this, node = root.second]() { return ExecNode(*node, nullptr); }));
    }
    for (const auto& future : futures)
      worker_pool_.Wait(future);
  }

  bool WasBuilt(Symbol output) {
    std::lock_guard<std::mutex> lock(state_mu_);
    return built_outputs_.count(output) != 0;
  }

 private:
  struct NodeState {
    bool processing;
    double timestamp;
    bool failed;
    std::thread::id owner;
  };

  bool IsFailed(Symbol output) {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto it = done_.find(output);
    return it != done_.end() && it->second.failed;
  }

  void AcquireJob() {
    std::unique_lock<std::mutex> lock(job_mu_);
    job_cv_.wait(lock, [this] { return active_jobs_ < job_limit_; });
    ++active_jobs_;
  }

  void ReleaseJob() {
    std::lock_guard<std::mutex> lock(job_mu_);
    --active_jobs_;
    job_cv_.notify_one();
  }

  void DeleteFailedOutput(const DepNode& node, double previous_ts) {
    if (node.delete_on_error && !node.precious &&
        GetTimestamp(node.output.str()) != previous_ts)
      unlink(node.output.c_str());
  }

  CommandEvaluator ce_;
  std::unordered_map<Symbol, NodeState> done_;
  std::unordered_set<Symbol> built_outputs_;
  // Protected by state_mu_.  Each entry describes the dependency currently
  // awaited by a worker, allowing parallel cycles to be diagnosed instead of
  // deadlocking the executor.
  std::unordered_map<std::thread::id, Symbol> waiting_on_;
  std::mutex state_mu_;
  std::condition_variable state_cv_;
  std::mutex eval_mu_;
  std::mutex output_mu_;
  std::mutex recursive_command_mu_;
  std::mutex job_mu_;
  std::condition_variable job_cv_;
  int job_limit_;
  int active_jobs_;
  std::string shell_;
  std::string shellflag_;
  uint64_t num_commands_;
  std::atomic<bool> needs_build_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> cancel_requested_{false};
  bool parallel_;
  WorkerPool worker_pool_;
};

}  // namespace

bool IsRecursiveKatiCommand(const std::string& command) {
  return IsRecursiveKatiCommandMarker(command);
}

ExecResult Exec(const std::vector<NamedDepNode>& roots,
                Evaluator* ev,
                bool parallel) {
  for (const auto& root : roots) {
    if (root.second->is_notparallel) {
      parallel = false;
      break;
    }
  }
  Executor executor(ev, parallel);
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
  if (executor.Count() == 0 && !g_flags.is_silent_mode &&
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
