// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef READY_QUEUE_H_
#define READY_QUEUE_H_

#include <cstddef>
#include <functional>
#include <vector>

// A resolved acyclic task graph. Workers never wait for prerequisite tasks:
// a shared coordinator makes tasks ready and records completion under one lock.
// The caller participates; both callbacks may run on bound worker threads.
struct ReadyTask {
  std::vector<size_t> dependencies;
  bool inline_work = false;
};

struct ReadyQueueStats {
  size_t workers = 0;
  size_t inline_tasks = 0;
  size_t peak_active = 0;
};

ReadyQueueStats RunReadyQueue(
    const std::vector<ReadyTask>& tasks,
    size_t jobs,
    const std::function<void(size_t)>& execute,
    const std::function<bool(size_t)>& can_inline = {},
    const std::function<void()>& cancel = {});

#endif  // READY_QUEUE_H_
