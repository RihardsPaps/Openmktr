// Copyright 2026 Rihards Paps and Haralds Paps
// SPDX-License-Identifier: MPL-2.0

#ifndef LOAD_LIMIT_H_
#define LOAD_LIMIT_H_

// A negative limit disables throttling. A negative sample means sampling
// failed: retain forward progress, but conservatively admit only one recipe at
// a time.
inline bool LoadAllowsJob(double limit, int active_jobs, double sample) {
  return limit < 0 || active_jobs == 0 || (sample >= 0 && sample < limit);
}

#endif  // LOAD_LIMIT_H_
