// FORK MODIFICATION NOTICE (2026)
// Resolve each thread's owned connection once per acquisition, without macros.
// Own invocation state and publish coherent generations with typed freshness.
// Extract shared FIFO reservations and load admission into a runtime service.
// Scope recipe reservations and close per-thread transport descriptors.
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

#include "jobserver.h"
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include "session.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#include <atomic>
#include <mutex>

#include "fileutil.h"
#include "load_limit.h"
#include "log.h"
#include "process.h"
#include "stats.h"

namespace {

struct JobserverConnection {
  JobserverConnection() = default;
  JobserverConnection(const JobserverConnection&) = delete;
  JobserverConnection& operator=(const JobserverConnection&) = delete;
  int fd = -1;
  std::string path;
  ~JobserverConnection() {
    if (fd >= 0)
      close(fd);
  }
};
constexpr int kReservedKatiJobToken = -2;
struct JobserverState {
  // Connections are indexed only while read_mu is held and remain alive
  // until all workers have joined and the session is destroyed.
  std::unordered_map<std::thread::id, JobserverConnection> connections;
  std::atomic<bool> reserved{false};
  std::mutex read_mu;
#if defined(__linux__)
  std::atomic<int> wakeup_fd{-1};
  bool initialized = false;
  ~JobserverState() {
    if (wakeup_fd >= 0)
      close(wakeup_fd);
  }
#endif
};

struct ProcessAdmission {
  std::mutex mutex;
  std::condition_variable available;
  int active = 0;
  bool load_warning = false;
};

// Called with state.read_mu held. The descriptor carries wakeups,
// never job tokens, and must not survive a recipe exec.
int ReservedKatiWakeupFd(JobserverState& state) {
#if defined(__linux__)
  if (!state.initialized) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd >= 0 && fd <= STDERR_FILENO) {
      const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      close(fd);
      fd = replacement;
    }
    state.wakeup_fd.store(fd, std::memory_order_release);
    state.initialized = true;
  }
  return state.wakeup_fd.load(std::memory_order_relaxed);
#else
  (void)state;
  return -1;
#endif
}

void WakeReservedKatiWaiters() {
#if defined(__linux__)
  const int fd = SessionService<JobserverState>().wakeup_fd.load(
      std::memory_order_acquire);
  const uint64_t notification = 1;
  if (fd >= 0 &&
      HANDLE_EINTR(write(fd, &notification, sizeof(notification))) < 0 &&
      errno != EAGAIN) {
    LOG("failed to notify reservation waiters");
  }
#endif
}

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
      SessionService<JobserverState>().reserved.compare_exchange_strong(
          expected, true))
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

  // One reader per process waits on the shared FIFO. Waking every local
  // recipe for one token only creates read/EAGAIN contention.
  auto& state = SessionService<JobserverState>();
  std::unique_lock<std::mutex> reader_lock(state.read_mu);
  auto& connection = state.connections[std::this_thread::get_id()];
  if (connection.fd >= 0 && connection.path != fifo) {
    close(connection.fd);
    connection.fd = -1;
    connection.path.clear();
  }

  if (connection.fd < 0) {
    connection.fd = OpenKatiJobserver();
    if (connection.fd < 0) {
      LOG("jobserver unavailable: %s", fifo);
      return -1;
    }
    connection.path = fifo;
    if (fcntl(connection.fd, F_SETFL, O_NONBLOCK) < 0)
      PERROR("fcntl(O_NONBLOCK) failed");
  }

  const char* inherited = getenv("KATI_JOBSERVER_RESERVED");
  const int wakeup_fd = inherited && strcmp(inherited, "1") == 0
                            ? ReservedKatiWakeupFd(state)
                            : -1;
  char token;
  while (true) {
    if (GetBuildSignal() != 0)
      return -1;
    // A previously busy inherited slot can become free while this thread
    // waits for FIFO capacity owned by another process.
    if (TryAcquireReservedKatiJobToken() == kReservedKatiJobToken)
      return kReservedKatiJobToken;
    struct pollfd ready[2] = {{connection.fd, POLLIN, 0},
                              {wakeup_fd, POLLIN, 0}};
    int available = poll(ready, wakeup_fd >= 0 ? 2 : 1, 100);
    if (available == 0 || (available < 0 && errno == EINTR))
      continue;
    if (available < 0)
      break;
    if (wakeup_fd >= 0 && (ready[1].revents & POLLIN)) {
      uint64_t notifications;
      HANDLE_EINTR(read(wakeup_fd, &notifications, sizeof(notifications)));
      continue;
    }
    ssize_t n = HANDLE_EINTR(read(connection.fd, &token, 1));
    if (n == 1)
      return connection.fd;
    if (n == 0)
      break;
    if (n < 0 && errno != EAGAIN)
      break;
  }

  close(connection.fd);
  connection.fd = -1;
  connection.path.clear();
  return -1;
}

void ReleaseKatiJobToken(int fd) {
  if (fd == kReservedKatiJobToken) {
    SessionService<JobserverState>().reserved.store(false);
    WakeReservedKatiWaiters();
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

JobLease::JobLease(bool acquire) {
  COLLECT_STATS("jobserver acquisition");
  token_ = acquire ? AcquireKatiJobToken() : TryAcquireReservedKatiJobToken();
}

JobLease::~JobLease() {
  ReleaseKatiJobToken(token_);
}

ProcessLease::ProcessLease() {
  auto& admission = SessionService<ProcessAdmission>();
  std::unique_lock<std::mutex> lock(admission.mutex);
  const int limit = std::max(1, g_flags.num_jobs);
  while (GetBuildSignal() == 0) {
    if (admission.active < limit) {
      double sample = -1;
      if (g_flags.load_limit >= 0 && admission.active > 0 &&
          !admission.load_warning && getloadavg(&sample, 1) != 1) {
        admission.load_warning = true;
        WARN(
            "omktr: cannot read load average; limiting this invocation to one "
            "process at a time");
      }
      if (LoadAllowsJob(g_flags.load_limit, admission.active, sample)) {
        ++admission.active;
        acquired_ = true;
        return;
      }
    }
    admission.available.wait_for(lock, std::chrono::milliseconds(100));
  }
}
ProcessLease::~ProcessLease() {
  if (!acquired_)
    return;
  auto& admission = SessionService<ProcessAdmission>();
  std::lock_guard<std::mutex> lock(admission.mutex);
  --admission.active;
  admission.available.notify_one();
}
