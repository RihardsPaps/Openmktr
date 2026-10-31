// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ready_queue.h"

#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include "session.h"

int main() {
  // Dependency depth consumes graph storage, not worker stacks or threads.
  std::vector<ReadyTask> chain(100000);
  for (size_t i = 1; i < chain.size(); ++i)
    chain[i].dependencies.push_back(i - 1);
  for (auto& task : chain)
    task.inline_work = true;
  size_t next = 0;
  const auto inline_stats =
      RunReadyQueue(chain, 8, [&](size_t id) { assert(id == next++); });
  assert(next == chain.size());
  assert(inline_stats.workers == 0);

  std::vector<ReadyTask> diamond(258);
  for (size_t i = 1; i < 257; ++i) {
    diamond[i].dependencies.push_back(0);
    diamond[257].dependencies.push_back(i);
  }
  for (size_t jobs : {1, 2, 8}) {
    std::vector<std::atomic<int>> visits(diamond.size());
    for (auto& count : visits)
      count.store(0);
    const auto stats = RunReadyQueue(diamond, jobs, [&](size_t id) {
      for (size_t dep : diamond[id].dependencies)
        assert(visits[dep].load() == 1);
      assert(visits[id].fetch_add(1) == 0);
    });
    assert(stats.workers <= jobs && stats.peak_active <= jobs);
    for (auto& count : visits)
      assert(count.load() == 1);
  }
  bool threw = false;
  try {
    RunReadyQueue(diamond, 8,
                  [](size_t) { throw std::runtime_error("worker failed"); });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);
  // No worker may outlive the exception or retain a reference to the callback.
  RunReadyQueue(diamond, 2, [](size_t) {});

  // The caller can be running a long task when a worker fails. Cancellation
  // must release that task before joining, and predicates retain the binding.
  {
    BuildSession session;
    std::mutex mutex;
    std::condition_variable cancelled;
    bool stop = false;
    std::atomic<size_t> predicates{0};
    bool caught = false;
    try {
      RunReadyQueue(
          std::vector<ReadyTask>(2), 2,
          [&](size_t id) {
            assert(BuildSession::Active() == &session);
            if (id == 1)
              throw std::runtime_error("cancel participant");
            std::unique_lock<std::mutex> lock(mutex);
            assert(cancelled.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return stop; }));
          },
          [&](size_t) {
            assert(BuildSession::Active() == &session);
            ++predicates;
            return false;
          },
          [&] {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            cancelled.notify_all();
          });
    } catch (const std::runtime_error&) {
      caught = true;
    }
    assert(caught && predicates == 2 && stop);
  }
  threw = false;
  try {
    RunReadyQueue({{{1}, false}, {{0}, false}}, 2, [](size_t) {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  assert(threw);
}
