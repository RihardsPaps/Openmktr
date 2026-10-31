// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ready_queue.h"

#include <assert.h>
#include <atomic>
#include <stdexcept>

int main() {
  // Dependency depth consumes graph storage, not worker stacks or threads.
  std::vector<ReadyTask> chain(100000);
  for (size_t i = 1; i < chain.size(); ++i)
    chain[i].dependencies.push_back(i - 1);
  for (auto& task : chain)
    task.inline_work = true;
  size_t next = 0;
  RunReadyQueue(chain, 8, [&](size_t id) { assert(id == next++); });
  assert(next == chain.size());

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
  threw = false;
  try {
    RunReadyQueue({{{1}, false}, {{0}, false}}, 2, [](size_t) {});
  } catch (const std::logic_error&) {
    threw = true;
  }
  assert(threw);
}
