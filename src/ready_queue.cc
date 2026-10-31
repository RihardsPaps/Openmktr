// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ready_queue.h"
#include "session.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

ReadyQueueStats RunReadyQueue(const std::vector<ReadyTask>& tasks,
                              size_t jobs,
                              const std::function<void(size_t)>& execute,
                              const std::function<bool(size_t)>& can_inline,
                              const std::function<void()>& cancel) {
  ReadyQueueStats stats;
  if (tasks.empty())
    return stats;
  jobs = std::max(size_t{1}, std::min(jobs, tasks.size()));
  std::vector<size_t> remaining;
  std::vector<std::vector<size_t>> dependents(tasks.size());
  std::deque<size_t> ready;
  for (size_t id = 0; id < tasks.size(); ++id) {
    remaining.push_back(tasks[id].dependencies.size());
    if (!remaining.back())
      ready.push_back(id);
    for (size_t dependency : tasks[id].dependencies) {
      if (dependency >= tasks.size())
        throw std::logic_error("invalid task dependency");
      dependents[dependency].push_back(id);
    }
  }

  // One shared coordinator owns readiness. A completing participant updates
  // it directly, avoiding a completion queue and a second thread round trip
  // before newly unblocked work can start. The caller also executes tasks.
  std::mutex mutex;
  std::condition_variable available;
  std::exception_ptr failure;
  bool stopping = false;
  size_t active = 0;
  size_t finished = 0;
  std::vector<std::thread> workers;
  BuildSession* session = BuildSession::Active();
  std::function<void()> work;
  work = [&]() {
    try {
      for (;;) {
        size_t id;
        {
          std::unique_lock<std::mutex> lock(mutex);
          available.wait(
              lock, [&] { return stopping || !ready.empty() || active == 0; });
          if (stopping)
            return;
          if (ready.empty())
            throw std::logic_error("cycle in ready task graph");
          id = ready.front();
          ready.pop_front();
          ++active;
          stats.peak_active = std::max(stats.peak_active, active);
        }
        const bool inline_work =
            tasks[id].inline_work || (can_inline && can_inline(id));
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (stopping)
            return;
          if (inline_work) {
            ++stats.inline_tasks;
          } else {
            // Pure metadata/no-op graphs create no threads. Expand the pool
            // only when real work and additional ready tasks can overlap.
            const size_t wanted =
                std::min(jobs - 1, workers.size() + ready.size());
            while (workers.size() < wanted) {
              workers.emplace_back([&] {
                SessionBinding binding(session);
                work();
              });
            }
          }
        }
        execute(id);
        bool wake;
        {
          std::lock_guard<std::mutex> lock(mutex);
          --active;
          ++finished;
          for (size_t dependent : dependents[id])
            if (--remaining[dependent] == 0)
              ready.push_front(dependent);
          if (finished == tasks.size())
            stopping = true;
          wake = !ready.empty() || active == 0 || stopping;
        }
        if (wake)
          available.notify_all();
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure)
          failure = std::current_exception();
        stopping = true;
      }
      available.notify_all();
      if (cancel)
        cancel();
    }
  };
  work();
  // No further participant can extend workers once stopping is set.
  for (auto& worker : workers)
    worker.join();
  if (failure)
    std::rethrow_exception(failure);
  stats.workers = workers.size();
  return stats;
}
