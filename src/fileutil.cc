// FORK MODIFICATION NOTICE (2026)
// Changed by the Openmktr fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Supervise Ninja interruption cleanup without a separate timestamp process.
// Atomically retain exact managed-workspace recipe identities.
// Validate recursive reuse, regeneration inputs, and shared recipe budgets.
// Preserve shell parsing of multi-option flags in recipe supervisors.
// Prevent recipe exports from duplicating a live jobserver reservation.
// Elide pure no-op commands for the default POSIX shell and flags.
// Validate inherited FIFO endpoints before using the shared job budget.
// Forward build interruptions, reap recipes, and identify changed outputs.
// Coordinate generated recipes' load admission with process-owned locks.
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

#include "fileutil.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <unordered_map>
#include <unordered_set>

#include "flags.h"
#include "load_limit.h"
#include "log.h"
#include "strutil.h"

extern char** environ;

bool Exists(std::string_view filename) {
  CHECK(filename.size() < PATH_MAX);
  struct stat st;
  if (stat(std::string(filename).c_str(), &st) < 0) {
    return false;
  }
  return true;
}

double GetTimestampFromStat(const struct stat& st) {
#if defined(__linux__)
  return st.st_mtime + st.st_mtim.tv_nsec * 0.001 * 0.001 * 0.001;
#else
  return st.st_mtime;
#endif
}

double GetTimestamp(std::string_view filename) {
  CHECK(filename.size() < PATH_MAX);
  struct stat st;
  if (stat(std::string(filename).c_str(), &st) < 0) {
    return -2.0;
  }
  return GetTimestampFromStat(st);
}

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

std::string GetOutputStamp(std::string_view path) {
  struct stat st;
  if (lstat(std::string(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return "missing";
#if defined(__APPLE__)
  const auto mtime = st.st_mtimespec;
#else
  const auto mtime = st.st_mtim;
#endif
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(mtime.tv_sec) + ":" + std::to_string(mtime.tv_nsec);
}

bool ReadNinjaRecipeState(const std::string& path, std::string* state) {
  FILE* stream = fopen(path.c_str(), "rb");
  if (!stream)
    return false;
  state->clear();
  char buffer[8192];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
    if (state->size() + count > 256 * 1024 * 1024) {
      fclose(stream);
      return false;
    }
    state->append(buffer, count);
  }
  const bool valid = !ferror(stream) && HasPrefix(*state, "OMKTR_RECIPE1\n");
  fclose(stream);
  return valid;
}

void WriteNinjaRecipeState(const std::string& path, const std::string& state) {
  std::string previous;
  if (ReadNinjaRecipeState(path, &previous) && previous == state)
    return;
  std::string temporary = path + ".tmp.XXXXXX";
  const int fd = mkstemp(temporary.data());
  if (fd < 0)
    PERROR("create Ninja recipe state failed");
  size_t written = 0;
  while (written < state.size()) {
    const ssize_t count =
        HANDLE_EINTR(write(fd, state.data() + written, state.size() - written));
    if (count <= 0)
      PERROR("write Ninja recipe state failed");
    written += count;
  }
  if (close(fd) != 0 || rename(temporary.c_str(), path.c_str()) != 0)
    PERROR("publish Ninja recipe state failed");
}

std::string GetFileIdentity(std::string_view path) {
  struct stat st;
  if (stat(std::string(path).c_str(), &st) != 0)
    return errno == ENOENT ? "missing" : "unreadable";
#if defined(__APPLE__)
  const auto mt = st.st_mtimespec;
  const auto ct = st.st_ctimespec;
#else
  const auto mt = st.st_mtim;
  const auto ct = st.st_ctim;
#endif
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(st.st_mode) + ":" + std::to_string(st.st_size) + ":" +
         std::to_string(mt.tv_sec) + ":" + std::to_string(mt.tv_nsec) + ":" +
         std::to_string(ct.tv_sec) + ":" + std::to_string(ct.tv_nsec);
}

void DeleteChangedOutput(std::string_view path, const std::string& previous) {
  const std::string current = GetOutputStamp(path);
  if (current != "missing" && current != previous)
    unlink(std::string(path).c_str());
}

namespace {

thread_local int kati_jobserver_fd = -1;
thread_local std::string kati_jobserver_path;
constexpr int kReservedKatiJobToken = -2;
std::atomic<bool> reserved_kati_job_in_use{false};

int OpenKatiJobserver() {
  const char* fifo = getenv("KATI_JOBSERVER_FIFO");
  if (fifo == nullptr || *fifo == '\0')
    return -1;

  // Check the opened object, not just its path. Nonblocking open also avoids
  // waiting on an invalid endpoint before its type can be checked.
  int fd = open(fifo, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISFIFO(st.st_mode) ||
      fcntl(fd, F_SETFL, 0) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int TryAcquireReservedKatiJobToken() {
  const char* reserved = getenv("KATI_JOBSERVER_RESERVED");
  bool expected = false;
  if (reserved != nullptr && strcmp(reserved, "1") == 0 &&
      reserved_kati_job_in_use.compare_exchange_strong(expected, true))
    return kReservedKatiJobToken;
  return -1;
}

}  // namespace

bool IsKatiJobserverAvailable() {
  // Do not consume a token: a valid jobserver may currently have none free.
  int fd = OpenKatiJobserver();
  if (fd < 0)
    return false;
  close(fd);
  return true;
}

int AcquireKatiJobToken() {
  const int reserved = TryAcquireReservedKatiJobToken();
  if (reserved == kReservedKatiJobToken)
    return reserved;

  const char* fifo = getenv("KATI_JOBSERVER_FIFO");
  if (fifo == nullptr || *fifo == '\0')
    return -1;

  if (kati_jobserver_fd >= 0 && kati_jobserver_path != fifo) {
    close(kati_jobserver_fd);
    kati_jobserver_fd = -1;
    kati_jobserver_path.clear();
  }

  if (kati_jobserver_fd < 0) {
    kati_jobserver_fd = OpenKatiJobserver();
    if (kati_jobserver_fd < 0) {
      LOG("jobserver unavailable: %s", fifo);
      return -1;
    }
    kati_jobserver_path = fifo;
    if (fcntl(kati_jobserver_fd, F_SETFL, O_NONBLOCK) < 0)
      PERROR("fcntl(O_NONBLOCK) failed");
  }

  char token;
  while (true) {
    if (GetBuildSignal() != 0)
      return -1;
    struct pollfd ready = {kati_jobserver_fd, POLLIN, 0};
    int available = poll(&ready, 1, 100);
    if (available == 0 || (available < 0 && errno == EINTR))
      continue;
    if (available < 0)
      break;
    ssize_t n = HANDLE_EINTR(read(kati_jobserver_fd, &token, 1));
    if (n == 1)
      return kati_jobserver_fd;
    if (n == 0)
      break;
    if (n < 0 && errno != EAGAIN)
      break;
  }

  close(kati_jobserver_fd);
  kati_jobserver_fd = -1;
  kati_jobserver_path.clear();
  return -1;
}

void ReleaseKatiJobToken(int fd) {
  if (fd == kReservedKatiJobToken) {
    reserved_kati_job_in_use.store(false);
    return;
  }
  if (fd < 0)
    return;

  const char token = 'x';
  if (HANDLE_EINTR(write(fd, &token, 1)) != 1) {
    LOG("failed to return jobserver token");
  }
}

int AcquireNinjaLoadSlot() {
  const char* path = getenv("KATI_NINJA_LOAD_STATE");
  const char* value = getenv("KATI_NINJA_LOAD_LIMIT");
  if (!path || !*path || !value || !*value)
    return -1;
  const double limit = ParseLoadLimit(value);
  const int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    PERROR("open Ninja load state failed");
  while (!GetBuildSignal()) {
    struct flock guard = {};
    guard.l_type = F_WRLCK;
    guard.l_whence = SEEK_SET;
    guard.l_len = 1;
    if (fcntl(fd, F_SETLK, &guard) == 0) {
      // Only the presence of another recipe matters for Make's progress
      // exception. Each helper locks its own PID byte; the kernel drops that
      // lock on exit, including forced termination, without stale counters.
      struct flock active = {};
      active.l_type = F_WRLCK;
      active.l_whence = SEEK_SET;
      active.l_start = 1;
      if (fcntl(fd, F_GETLK, &active) < 0)
        PERROR("query Ninja load slots failed");
      char failed = 0;
      if (pread(fd, &failed, 1, 0) != 1)
        PERROR("read Ninja load state failed");
      double sample = -1;
      if (active.l_type != F_UNLCK && !failed && getloadavg(&sample, 1) != 1) {
        sample = -1;
        failed = 1;
        if (pwrite(fd, &failed, 1, 0) != 1)
          PERROR("write Ninja load state failed");
        fprintf(stderr,
                "omktr: cannot read load average; limiting this executor "
                "to one recipe at a time\n");
      }
      const bool admitted =
          LoadAllowsJob(limit, active.l_type == F_UNLCK ? 0 : 1, sample);
      if (admitted) {
        struct flock slot = {};
        slot.l_type = F_WRLCK;
        slot.l_whence = SEEK_SET;
        slot.l_start = static_cast<off_t>(getpid()) + 1;
        slot.l_len = 1;
        if (fcntl(fd, F_SETLK, &slot) < 0)
          PERROR("lock Ninja load slot failed");
      }
      guard.l_type = F_UNLCK;
      if (fcntl(fd, F_SETLK, &guard) < 0)
        PERROR("unlock Ninja load admission failed");
      if (admitted)
        return fd;
    } else if (errno != EACCES && errno != EAGAIN && errno != EINTR) {
      PERROR("lock Ninja load admission failed");
    }
    // A changing load need not coincide with a completed job. Poll without
    // owning the admission lock or a FIFO reservation, and notice signals.
    poll(nullptr, 0, 100);
  }
  close(fd);
  return -1;
}

int RunCommand(const std::string& shell,
               const std::string& shellflag,
               const std::string& cmd,
               RedirectStderr redirect_stderr,
               std::string* s,
               bool acquire_job_token,
               const std::function<void(std::string_view)>& on_output,
               int make_level,
               const std::vector<std::string>* extra_env,
               const std::vector<std::string>* unset_env,
               std::string_view cleanup_output) {
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
  const int job_token = acquire_job_token ? AcquireKatiJobToken()
                                          : TryAcquireReservedKatiJobToken();
  if (GetBuildSignal() != 0) {
    ReleaseKatiJobToken(job_token);
    return GetBuildSignal();
  }
  const char* fifo = getenv("KATI_JOBSERVER_FIFO");
  const bool forward_slot = (fifo != nullptr && *fifo != '\0') ||
                            getenv("KATI_JOBSERVER_RESERVED") != nullptr;
  const std::string reserved_env =
      forward_slot ? std::string("KATI_JOBSERVER_RESERVED=") +
                         (job_token == -1 ? "0" : "1")
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
  std::string recipe_path;
  std::string recipe_command;
  if (cmd.size() > 120000) {
    char path[] = "/tmp/omktr-recipe-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
      PERROR("mkstemp failed");
    recipe_path = path;
    size_t written = 0;
    while (written < cmd.size()) {
      ssize_t count =
          HANDLE_EINTR(write(fd, cmd.data() + written, cmd.size() - written));
      if (count <= 0)
        PERROR("write recipe failed");
      written += count;
    }
    if (close(fd) != 0)
      PERROR("close recipe failed");
    recipe_command = ". " + recipe_path;
  }
  const std::string& shell_command = recipe_path.empty() ? cmd : recipe_command;
  const char* argv[] = {NULL, NULL, NULL, NULL};
  std::string cmd_with_shell;
  if (shell[0] != '/' || shell.find_first_of(" $") != std::string::npos ||
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

  int pipefd[2];
  if (pipe(pipefd) != 0)
    PERROR("pipe failed");
  if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0)
    PERROR("fcntl(FD_CLOEXEC) failed");

  posix_spawn_file_actions_t action;
  int err = posix_spawn_file_actions_init(&action);
  if (err != 0) {
    ERROR("posix_spawn_file_actions_init: %s", strerror(err));
  }

  err = posix_spawn_file_actions_addclose(&action, pipefd[0]);
  if (err != 0) {
    ERROR("posix_spawn_file_actions_addclose: %s", strerror(err));
  }

  if (redirect_stderr == RedirectStderr::STDOUT) {
    err = posix_spawn_file_actions_adddup2(&action, pipefd[1], 2);
    if (err != 0) {
      ERROR("posix_spawn_file_actions_adddup2: %s", strerror(err));
    }
  } else if (redirect_stderr == RedirectStderr::DEV_NULL) {
    err =
        posix_spawn_file_actions_addopen(&action, 2, "/dev/null", O_WRONLY, 0);
    if (err != 0) {
      ERROR("posix_spawn_file_actions_addopen: %s", strerror(err));
    }
  }
  err = posix_spawn_file_actions_adddup2(&action, pipefd[1], 1);
  if (err != 0) {
    ERROR("posix_spawn_file_actions_adddup2: %s", strerror(err));
  }
  err = posix_spawn_file_actions_addclose(&action, pipefd[1]);
  if (err != 0) {
    ERROR("posix_spawn_file_actions_addclose: %s", strerror(err));
  }

  posix_spawnattr_t attr;
  err = posix_spawnattr_init(&attr);
  if (err != 0) {
    ERROR("posix_spawnattr_init: %s", strerror(err));
  }

  short flags = 0;
  if (build_signals_active) {
    // A signal addressed to Openmktr alone must reach all recipe descendants.
    // Recursive Openmktr jobs forward it to their own recipe groups in turn.
    flags |= POSIX_SPAWN_SETPGROUP;
    if (posix_spawnattr_setpgroup(&attr, 0) != 0)
      PERROR("posix_spawnattr_setpgroup failed");
  }
#ifdef POSIX_SPAWN_USEVFORK
  flags |= POSIX_SPAWN_USEVFORK;
#endif

  err = posix_spawnattr_setflags(&attr, flags);
  if (err != 0) {
    ERROR("posix_spawnattr_setflags: %s", strerror(err));
  }

  pid_t pid;
  const std::string output_stamp =
      cleanup_output.empty() ? "" : GetOutputStamp(cleanup_output);
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
      ERROR(
          "posix_spawn: %s (arguments %zu bytes, environment %zu bytes, "
          "largest environment entry %zu bytes)",
          strerror(err), argument_bytes, environment_bytes,
          largest_environment_entry);
    }
    ERROR("posix_spawn: %s", strerror(err));
  }

  err = posix_spawnattr_destroy(&attr);
  if (err != 0) {
    ERROR("posix_spawnattr_destroy: %s", strerror(err));
  }
  err = posix_spawn_file_actions_destroy(&action);
  if (err != 0) {
    ERROR("posix_spawn_file_actions_destroy: %s", strerror(err));
  }

  int status;
  close(pipefd[1]);
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
      PERROR("poll failed");
    char buf[4096];
    ssize_t r = HANDLE_EINTR(read(pipefd[0], buf, sizeof(buf)));
    if (r < 0)
      PERROR("read failed");
    if (r == 0)
      break;
    if (on_output)
      on_output(std::string_view(buf, r));
    else
      s->append(buf, buf + r);
  }
  close(pipefd[0]);
  while (true) {
    forward_signal();
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      break;
    if (waited < 0 && errno != EINTR)
      PERROR("waitpid failed");
    poll(nullptr, 0, 10);
  }
  if (!recipe_path.empty() && unlink(recipe_path.c_str()) != 0)
    PERROR("unlink recipe failed");

  // Shell traps used by older generated graphs cleaned up these four signal
  // statuses, including explicit `exit 130` and similar recipe failures.
  // Keep that behavior even if a recipe installs its own traps or uses exec.
  const int code = WIFEXITED(status)     ? WEXITSTATUS(status)
                   : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                         : 0;
  if (!cleanup_output.empty() && (GetBuildSignal() != 0 || code == 129 ||
                                  code == 130 || code == 131 || code == 143))
    DeleteChangedOutput(cleanup_output, output_stamp);

  ReleaseKatiJobToken(job_token);
  return status;
}

std::string GetExecutablePath() {
#if defined(__linux__)
  char mypath[PATH_MAX + 1];
  ssize_t l = readlink("/proc/self/exe", mypath, PATH_MAX);
  if (l < 0) {
    PERROR("readlink for /proc/self/exe");
  }
  mypath[l] = '\0';
  return {mypath};
#elif defined(__APPLE__)
  char mypath[PATH_MAX + 1];
  uint32_t size = PATH_MAX;
  if (_NSGetExecutablePath(mypath, &size) != 0) {
    ERROR("_NSGetExecutablePath failed");
  }
  mypath[size] = 0;
  return {mypath};
#else
#error "Unsupported OS"
#endif
}

namespace {

class GlobCache {
 public:
  ~GlobCache() { Clear(); }
  const GlobMap::mapped_type& Get(std::string_view pat) {
    // WordScanner returns views into a larger expansion buffer.  Materialize
    // the cache key before calling any C API: pat.data() is not required to
    // point at a NUL-terminated string.
    std::string key(pat);
    auto [it, inserted] = cache_.try_emplace(key);
    auto& files = it->second;
    if (inserted) {
      if (strcspn(key.c_str(), "?*[\\") != key.size()) {
        glob_t gl = {};
        int result = glob(key.c_str(), 0, NULL, &gl);
        // glob() may fail with GLOB_NOMATCH, GLOB_ABORTED, or GLOB_NOSPACE.
        // Its output is not valid on failure, so never inspect gl_pathc or
        // gl_pathv unless the call succeeded.  A failed wildcard expands to
        // no files, matching the behavior expected by make wildcard users.
        if (result == 0) {
          for (size_t i = 0; i < gl.gl_pathc; i++)
            files.push_back(gl.gl_pathv[i]);
        }
        globfree(&gl);
      } else {
        if (Exists(key))
          files.push_back(key);
      }
    }
    return files;
  }

  const GlobMap& GetAll() const { return cache_; }

  void Clear() { cache_.clear(); }

 private:
  GlobMap cache_;
};

static GlobCache g_gc;

}  // namespace

const GlobMap::mapped_type& Glob(std::string_view pat) {
  return g_gc.Get(pat);
}

const GlobMap& GetAllGlobCache() {
  return g_gc.GetAll();
}

void ClearGlobCache() {
  g_gc.Clear();
}
