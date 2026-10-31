// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <assert.h>
#include <time.h>
#include "session.h"

#include "flags.h"
#include "stats.h"
#include "timeutil.h"

namespace {
bool simulated = false;
time_t wall = 1000;
time_t monotonic = 10;
}  // namespace

extern "C" int __real_clock_gettime(clockid_t clock, struct timespec* value);
extern "C" int __wrap_clock_gettime(clockid_t clock, struct timespec* value) {
  if (!simulated)
    return __real_clock_gettime(clock, value);
  assert(clock == CLOCK_REALTIME || clock == CLOCK_MONOTONIC);
  value->tv_sec = clock == CLOCK_REALTIME ? wall : monotonic;
  value->tv_nsec = 0;
  return 0;
}

int main() {
  BuildSession session;
  simulated = true;
  assert(GetWallTime() == 1000);
  assert(GetMonotonicTime() == 10);
  g_flags.enable_stat_logs = true;
  Stats& stats = SessionStats("clock regression");
  {
    ScopedStatsRecorder record(&stats);
    wall -= 600;
    monotonic += 2;
  }
  assert(GetWallTime() == 400);
  assert(stats.Json().find("\"seconds\":2.000000000") != std::string::npos);
  // Reporting is observational: a second report cannot use freed registry
  // storage, and subsequent measurement retains a valid registration.
  ReportAllStats();
  ReportAllStats();
  {
    ScopedStatsRecorder record(&stats);
    wall += 3600;
    monotonic += 3;
  }
  assert(stats.Json().find("\"seconds\":5.000000000") != std::string::npos);
  simulated = false;
}
