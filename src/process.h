// FORK MODIFICATION NOTICE (2026)
// Expose process supervision independently from filesystem observations.
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

#ifndef PROCESS_H_
#define PROCESS_H_

#include <signal.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Installed before executor workers start and restored after they stop.
// The handler only records a signal; recipe waiters forward it and reap jobs.
class ScopedBuildSignals {
 public:
  ScopedBuildSignals();
  ~ScopedBuildSignals();
  ScopedBuildSignals(const ScopedBuildSignals&) = delete;
  ScopedBuildSignals& operator=(const ScopedBuildSignals&) = delete;

 private:
  std::vector<struct sigaction> previous_;
};

int GetBuildSignal();
void CancelBuildProcesses();
enum struct RedirectStderr {
  NONE,
  STDOUT,
  DEV_NULL,
};

// Returns a POSIX wait status. Launch errors are ordinary failed commands
// (127 for a missing executable, 126 for infrastructure failures).
// A nonempty cleanup_output is stamped after job admission and removed only
// when interruption changes that regular file. Empty preserves ordinary calls.
int RunCommand(const std::string& shell,
               const std::string& shellflag,
               const std::string& cmd,
               RedirectStderr redirect_stderr,
               std::string* out,
               bool acquire_job_token = true,
               const std::function<void(std::string_view)>& on_output = {},
               int make_level = -1,
               const std::vector<std::string>* extra_env = nullptr,
               const std::vector<std::string>* unset_env = nullptr,
               std::string_view cleanup_output = {});

#endif  // PROCESS_H_
