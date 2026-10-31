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

  std::mutex mutex;
  std::condition_variable work_available;
  std::condition_variable completion_available;
  std::deque<size_t> dispatched;
  std::deque<size_t> completed;
  std::exception_ptr failure;
  bool stopping = false;
  size_t running = 0;
  std::vector<std::thread> workers;
  auto stop = [&]() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    work_available.notify_all();
    for (auto& worker : workers)
      worker.join();
  };
  BuildSession* session = BuildSession::Active();
  auto work = [&]() {
    SessionBinding binding(session);
    for (;;) {
      size_t id;
      {
        std::unique_lock<std::mutex> lock(mutex);
        work_available.wait(lock,
                            [&]() { return stopping || !dispatched.empty(); });
        if (stopping)
          return;
        id = dispatched.front();
        dispatched.pop_front();
        ++running;
        stats.peak_active = std::max(stats.peak_active, running);
      }
      try {
        execute(id);
      } catch (...) {
        if (cancel)
          cancel();
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure)
          failure = std::current_exception();
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        --running;
        completed.push_back(id);
      }
      completion_available.notify_one();
    }
  };
  size_t active = 0;
  size_t finished = 0;
  auto finish = [&](size_t id) {
    ++finished;
    for (size_t dependent : dependents[id])
      if (--remaining[dependent] == 0)
        // Start newly unblocked work before walking the remaining source
        // leaves. This overlaps metadata checks with already-ready recipes.
        ready.push_front(dependent);
  };
  try {
    while (finished != tasks.size()) {
      while (!ready.empty()) {
        const size_t id = ready.front();
        const bool inline_work =
            tasks[id].inline_work || (can_inline && can_inline(id));
        // Keep one bounded batch ready for workers to take immediately after
        // a completion. Waiting for the coordinator to wake and dispatch each
        // individual recipe otherwise leaves a gap between short processes.
        if (!inline_work && active == jobs * 2)
          break;
        ready.pop_front();
        if (inline_work) {
          ++stats.inline_tasks;
          execute(id);
          finish(id);
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          dispatched.push_back(id);
          ++active;
          if (workers.size() < std::min(active, jobs))
            workers.emplace_back(work);
        }
        work_available.notify_one();
      }
      if (finished == tasks.size())
        break;
      if (!active)
        throw std::logic_error("cycle in ready task graph");
      std::deque<size_t> batch;
      {
        std::unique_lock<std::mutex> lock(mutex);
        completion_available.wait(lock, [&]() { return !completed.empty(); });
        batch.swap(completed);
        if (failure)
          std::rethrow_exception(failure);
      }
      for (size_t id : batch) {
        --active;
        finish(id);
      }
    }
  } catch (...) {
    if (cancel)
      cancel();
    stop();
    throw;
  }
  stop();
  stats.workers = workers.size();
  return stats;
}
