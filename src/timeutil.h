// FORK MODIFICATION NOTICE (2026)
// Refactor runtime service ownership and diagnostic timing for Openmktr.
// Fork changes are covered by MPL-2.0; see docs/LICENSING.md and NOTICE.
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

#ifndef TIMEUTIL_H_
#define TIMEUTIL_H_

// Wall time is used only when comparing with filesystem timestamps.
double GetWallTime();
// Elapsed durations must not change when the system clock is corrected.
double GetMonotonicTime();

struct ScopedTimeReporter {
 public:
  explicit ScopedTimeReporter(const char* name);
  ~ScopedTimeReporter();

 private:
  const char* name_;
  double start_;
};

#endif  // TIME_H_
