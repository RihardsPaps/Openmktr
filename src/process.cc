// FORK MODIFICATION NOTICE (2026)
// Extract process supervision from filesystem observation. Reap failed
// launches and return job leases on infrastructure errors; preserve shell
// execution, streaming output, cancellation, and cleanup behavior.
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

#include "process.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <algorithm>
#include <atomic>
#include <system_error>
#include <unordered_set>

#include "fileutil.h"
#include "jobserver.h"
#include "log.h"
#include "stats.h"
#include "strutil.h"

extern char** environ;

namespace {

constexpr int kBuildSignals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
std::atomic<int> build_signal{0};
static_assert(std::atomic<int>::is_always_lock_free);
bool build_signals_active = false;

}  // namespace

ScopedBuildSignals::ScopedBuildSignals() {
  build_signal.store(0, std::memory_order_relaxed);
  for (int sig : kBuildSignals) {
    struct sigaction previous;
    if (sigaction(sig, nullptr, &previous) != 0)
      PERROR("sigaction failed");
    previous_.push_back(previous);
    if (previous.sa_handler == SIG_IGN)
      continue;
    struct sigaction action = {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = [](int signal) {
      build_signal.store(signal, std::memory_order_relaxed);
    };
    if (sigaction(sig, &action, nullptr) != 0)
      PERROR("sigaction failed");
  }
  build_signals_active = true;
}

ScopedBuildSignals::~ScopedBuildSignals() {
  build_signals_active = false;
  for (size_t i = 0; i < previous_.size(); ++i)
    if (sigaction(kBuildSignals[i], &previous_[i], nullptr) != 0)
      PERROR("sigaction failed");
  build_signal.store(0, std::memory_order_relaxed);
}

int GetBuildSignal() {
  return build_signal.load(std::memory_order_relaxed);
}

namespace {

// Every partially constructed launch has one owner. In particular, failures
// after spawn reap the child before its job lease can be returned.
struct ProcessResources {
  int pipe[2] = {-1, -1};
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  bool actions_ready = false;
  bool attributes_ready = false;
  pid_t child = -1;
  bool process_group = false;
  std::string script;
  std::string cleanup_output;
  std::string output_stamp;
  int script_fd = -1;
  int process_fd = -1;

  ~ProcessResources() {
    const int saved_errno = errno;
    if (child > 0) {
      kill(process_group ? -child : child, SIGKILL);
      int status;
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      if (!cleanup_output.empty())
        DeleteChangedOutput(cleanup_output, output_stamp);
    }
    for (int fd : pipe)
      if (fd >= 0)
        close(fd);
    if (script_fd >= 0)
      close(script_fd);
    if (process_fd >= 0)
      close(process_fd);
    if (attributes_ready)
      posix_spawnattr_destroy(&attributes);
    if (actions_ready)
      posix_spawn_file_actions_destroy(&actions);
    if (!script.empty())
      unlink(script.c_str());
    errno = saved_errno;
  }
};

void CheckProcessCall(int error, const char* operation) {
  if (error)
    throw std::system_error(error, std::generic_category(), operation);
}

int RunCommandImpl(const std::string& shell,
                   const std::string& shellflag,
                   const std::string& cmd,
                   RedirectStderr redirect_stderr,
                   std::string* s,
                   bool acquire_job_token,
                   const std::function<void(std::string_view)>& on_output,
                   int make_level,
                   const std::vector<std::string>* extra_env,
                   const std::vector<std::string>* unset_env,
                   std::string_view cleanup_output,
                   bool* child_interrupted) {
  if (GetBuildSignal() != 0)
    return GetBuildSignal();
  // Kbuild uses thousands of dummy recipes. The default shell's ':' has no
  // expansion, output, or side effects, so it needs neither a process nor a
  // jobserver token. Custom interpreters/flags can have observable behavior.
  if (shell == "/bin/sh" && shellflag == "-c" && cmd == ":")
    return 0;

  // A recipe owns one jobserver slot while its shell runs. Pass that slot to
  // nested Kati processes started indirectly by scripts or configure tests.
  // An explicitly recursive boundary can pass along an inherited reservation
  // without taking an additional FIFO token.
  JobLease job(acquire_job_token);
  if (GetBuildSignal() != 0) {
    return GetBuildSignal();
  }
  ScopedFilesystemEffect effect;
  ProcessResources resources;
  const char* fifo = getenv("KATI_JOBSERVER_FIFO");
  const bool forward_slot = (fifo != nullptr && *fifo != '\0') ||
                            getenv("KATI_JOBSERVER_RESERVED") != nullptr;
  const std::string reserved_env =
      forward_slot ? std::string("KATI_JOBSERVER_RESERVED=") +
                         (job.reserved() ? "1" : "0")
                   : "";
  std::vector<char*> child_env;
  const std::string level_env = "MAKELEVEL=" + std::to_string(make_level);
  const bool custom_env = forward_slot || make_level >= 0 ||
                          extra_env != nullptr || unset_env != nullptr;
  if (custom_env) {
    std::unordered_set<std::string_view> replaced;
    if (extra_env != nullptr)
      for (const std::string& entry : *extra_env)
        replaced.insert(std::string_view(entry).substr(0, entry.find('=')));
    if (unset_env != nullptr)
      for (const std::string& name : *unset_env)
        replaced.insert(name);
    for (char** entry = environ; *entry != nullptr; ++entry) {
      const std::string_view inherited(*entry);
      if (replaced.count(inherited.substr(0, inherited.find('='))) != 0)
        continue;
      if (forward_slot && strncmp(*entry, "KATI_JOBSERVER_RESERVED=", 24) == 0)
        continue;
      if (make_level >= 0 && strncmp(*entry, "MAKELEVEL=", 10) == 0)
        continue;
      child_env.push_back(*entry);
    }
    if (forward_slot)
      child_env.push_back(const_cast<char*>(reserved_env.c_str()));
    if (make_level >= 0)
      child_env.push_back(const_cast<char*>(level_env.c_str()));
    if (extra_env != nullptr) {
      for (const std::string& entry : *extra_env) {
        // Reservation ownership is assigned above for this particular child.
        // A make export must not add a second, stale copy of the variable.
        if (forward_slot && HasPrefix(entry, "KATI_JOBSERVER_RESERVED="))
          continue;
        child_env.push_back(const_cast<char*>(entry.c_str()));
      }
    }
    child_env.push_back(nullptr);
  }
  // Linux also limits each individual argument (MAX_ARG_STRLEN), even when
  // the complete argv and environment fit within ARG_MAX.  Generated recipes
  // can exceed that limit.  Source a private temporary file inside the same
  // shell so flags, variables, and control flow retain their usual meaning.
  std::string& recipe_path = resources.script;
  std::string recipe_command;
  if (cmd.size() > 120000) {
    char path[] = "/tmp/omktr-recipe-XXXXXX";
    int& fd = resources.script_fd;
#if defined(__linux__)
    fd = mkostemp(path, O_CLOEXEC);
#else
    fd = mkstemp(path);
#endif
    if (fd < 0)
      CheckProcessCall(errno, "mkstemp failed");
    recipe_path = path;
    size_t written = 0;
    while (written < cmd.size()) {
      ssize_t count =
          HANDLE_EINTR(write(fd, cmd.data() + written, cmd.size() - written));
      if (count <= 0) {
        const int error = count == 0 ? EIO : errno;
        CheckProcessCall(error, "write recipe failed");
      }
      written += count;
    }
    const int close_result = close(fd);
    fd = -1;
    if (close_result != 0)
      CheckProcessCall(errno, "close recipe failed");
    recipe_command = ". " + recipe_path;
  }
  const std::string& shell_command = recipe_path.empty() ? cmd : recipe_command;
  const char* argv[] = {NULL, NULL, NULL, NULL};
  std::string cmd_with_shell;
  if (shell.empty() || shell[0] != '/' ||
      shell.find_first_of(" $") != std::string::npos ||
      shellflag.find_first_of(" \t\r\n'\"\\$`") != std::string::npos) {
    // A flag string can contain several shell words (for example -e -c),
    // quoting, or expansions. Let the POSIX shell parse those just as it did
    // in the generated invocation before the jobserver supervisor was added.
    std::string cmd_escaped = shell_command;
    EscapeShell(&cmd_escaped);
    cmd_with_shell = shell + " " + shellflag + " \"" + cmd_escaped + "\"";
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = cmd_with_shell.c_str();
  } else {
    // If the shell isn't complicated, we don't need to wrap in /bin/sh
    argv[0] = shell.c_str();
    argv[1] = shellflag.c_str();
    argv[2] = shell_command.c_str();
  }

  auto& pipefd = resources.pipe;
#if defined(__linux__)
  // Parallel spawns must never inherit another recipe's pipe ends. Setting
  // FD_CLOEXEC after pipe() leaves a window where a sibling keeps the writer
  // alive, preventing EOF even after the intended child has exited.
  if (pipe2(pipefd, O_CLOEXEC) != 0)
    CheckProcessCall(errno, "pipe2 failed");
#else
  if (pipe(pipefd) != 0)
    CheckProcessCall(errno, "pipe failed");
  if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0)
    CheckProcessCall(errno, "fcntl(FD_CLOEXEC) failed");
#endif

  auto& action = resources.actions;
  int err = posix_spawn_file_actions_init(&action);
  if (err != 0) {
    CheckProcessCall(err, "posix_spawn_file_actions_init");
  }

  resources.actions_ready = true;
  err = posix_spawn_file_actions_addclose(&action, pipefd[0]);
  if (err != 0) {
    CheckProcessCall(err, "posix_spawn_file_actions_addclose");
  }

  if (redirect_stderr == RedirectStderr::STDOUT) {
    err = posix_spawn_file_actions_adddup2(&action, pipefd[1], 2);
    if (err != 0) {
      CheckProcessCall(err, "posix_spawn_file_actions_adddup2");
    }
  } else if (redirect_stderr == RedirectStderr::DEV_NULL) {
    err =
        posix_spawn_file_actions_addopen(&action, 2, "/dev/null", O_WRONLY, 0);
    if (err != 0) {
      CheckProcessCall(err, "posix_spawn_file_actions_addopen");
    }
  }
  err = posix_spawn_file_actions_adddup2(&action, pipefd[1], 1);
  if (err != 0) {
    CheckProcessCall(err, "posix_spawn_file_actions_adddup2");
  }
  err = posix_spawn_file_actions_addclose(&action, pipefd[1]);
  if (err != 0) {
    CheckProcessCall(err, "posix_spawn_file_actions_addclose");
  }

  auto& attr = resources.attributes;
  err = posix_spawnattr_init(&attr);
  if (err != 0) {
    CheckProcessCall(err, "posix_spawnattr_init");
  }

  resources.attributes_ready = true;
  short flags = 0;
  if (build_signals_active) {
    // A signal addressed to Openmktr alone must reach all recipe descendants.
    // Recursive Openmktr jobs forward it to their own recipe groups in turn.
    flags |= POSIX_SPAWN_SETPGROUP;
    CheckProcessCall(posix_spawnattr_setpgroup(&attr, 0),
                     "posix_spawnattr_setpgroup");
    resources.process_group = true;
  }
#ifdef POSIX_SPAWN_USEVFORK
  flags |= POSIX_SPAWN_USEVFORK;
#endif

  err = posix_spawnattr_setflags(&attr, flags);
  if (err != 0) {
    CheckProcessCall(err, "posix_spawnattr_setflags");
  }

  pid_t& pid = resources.child;
  resources.cleanup_output = cleanup_output;
  resources.output_stamp =
      cleanup_output.empty() ? "" : GetOutputStamp(cleanup_output);
  RecordStatCounter("process_launch_attempts");
  err = posix_spawn(&pid, argv[0], &action, &attr, const_cast<char**>(argv),
                    custom_env ? child_env.data() : environ);
  if (err != 0) {
    if (!recipe_path.empty())
      unlink(recipe_path.c_str());
    if (err == E2BIG) {
      size_t argument_bytes = 0;
      for (const char* argument : argv)
        if (argument != nullptr)
          argument_bytes += strlen(argument) + 1;
      size_t environment_bytes = 0;
      size_t largest_environment_entry = 0;
      char* const* environment = custom_env ? child_env.data() : environ;
      for (char* const* entry = environment; *entry != nullptr; ++entry) {
        const size_t length = strlen(*entry) + 1;
        environment_bytes += length;
        largest_environment_entry = std::max(largest_environment_entry, length);
      }
      WARN(
          "posix_spawn: %s (arguments %zu bytes, environment %zu bytes, "
          "largest environment entry %zu bytes)",
          strerror(err), argument_bytes, environment_bytes,
          largest_environment_entry);
    } else {
      WARN("posix_spawn: %s", strerror(err));
    }
    pid = -1;
    // A launch failure is a recipe failure, not a process-wide exit. Returning
    // through the lease also lets recursive siblings recover shared capacity.
    return (err == ENOENT ? 127 : 126) << 8;
  }

  *child_interrupted = true;
  // Release launch-only allocations while the child is running, rather than
  // lengthening the interval between its completion and the next admission.
  err = posix_spawnattr_destroy(&attr);
  resources.attributes_ready = false;
  CheckProcessCall(err, "posix_spawnattr_destroy");
  err = posix_spawn_file_actions_destroy(&action);
  resources.actions_ready = false;
  CheckProcessCall(err, "posix_spawn_file_actions_destroy");
  int status;
  close(pipefd[1]);
  pipefd[1] = -1;
  int forwarded_signal = 0;
  auto forward_signal = [&]() {
    const int sig = GetBuildSignal();
    if (sig != 0 && sig != forwarded_signal) {
      kill(-pid, sig);
      forwarded_signal = sig;
    }
  };
  while (true) {
    forward_signal();
    struct pollfd ready = {pipefd[0], POLLIN, 0};
    int available = poll(&ready, 1, 100);
    if (available == 0 || (available < 0 && errno == EINTR))
      continue;
    if (available < 0)
      CheckProcessCall(errno, "poll failed");
    char buf[4096];
    ssize_t r = HANDLE_EINTR(read(pipefd[0], buf, sizeof(buf)));
    if (r < 0)
      CheckProcessCall(errno, "read failed");
    if (r == 0)
      break;
    if (on_output)
      on_output(std::string_view(buf, r));
    else
      s->append(buf, buf + r);
  }
  close(pipefd[0]);
  pipefd[0] = -1;
  int& process_fd = resources.process_fd;
  bool process_fd_attempted = false;
  while (true) {
    forward_signal();
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      pid = -1;
      *child_interrupted = false;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      CheckProcessCall(errno, "waitpid failed");
    }
#if defined(__linux__) && defined(SYS_pidfd_open)
    // A shell can close stdout just before exiting. Wake on process exit
    // without adding 10 ms, but do not open a descriptor for reaped children.
    // Older kernels and restricted containers retain the portable fallback.
    if (!process_fd_attempted) {
      process_fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
      process_fd_attempted = true;
    }
#else
    (void)process_fd_attempted;
#endif
    struct pollfd exited = {process_fd, POLLIN, 0};
    if (poll(process_fd >= 0 ? &exited : nullptr, process_fd >= 0 ? 1 : 0, 10) <
            0 &&
        errno != EINTR) {
      CheckProcessCall(errno, "poll process failed");
    }
  }
  if (!recipe_path.empty() && unlink(recipe_path.c_str()) != 0)
    CheckProcessCall(errno, "unlink recipe failed");

  // Shell traps used by older generated graphs cleaned up these four signal
  // statuses, including explicit `exit 130` and similar recipe failures.
  // Keep that behavior even if a recipe installs its own traps or uses exec.
  const int code = WIFEXITED(status)     ? WEXITSTATUS(status)
                   : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                         : 0;
  if (!cleanup_output.empty() && (GetBuildSignal() != 0 || code == 129 ||
                                  code == 130 || code == 131 || code == 143))
    DeleteChangedOutput(cleanup_output, resources.output_stamp);

  return status;
}

}  // namespace

int RunCommand(const std::string& shell,
               const std::string& shellflag,
               const std::string& cmd,
               RedirectStderr redirect_stderr,
               std::string* out,
               bool acquire_job_token,
               const std::function<void(std::string_view)>& on_output,
               int make_level,
               const std::vector<std::string>* extra_env,
               const std::vector<std::string>* unset_env,
               std::string_view cleanup_output) {
  bool child_interrupted = false;
  try {
    return RunCommandImpl(shell, shellflag, cmd, redirect_stderr, out,
                          acquire_job_token, on_output, make_level, extra_env,
                          unset_env, cleanup_output, &child_interrupted);
  } catch (const std::system_error& error) {
    WARN("recipe infrastructure failure: %s", error.what());
    return child_interrupted ? SIGKILL : 126 << 8;
  }
}
