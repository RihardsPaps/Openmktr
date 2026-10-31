// FORK MODIFICATION NOTICE (2026)
// Own invocation state and publish coherent generations with typed freshness.
// Use monotonic durations and retain the registry across repeated reports.
// Fork changes are covered by MPL-2.0; see docs/LICENSING.md.
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

// +build ignore

#include "stats.h"
#include "session.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "find.h"
#include "flags.h"
#include "log.h"
#include "stringprintf.h"
#include "timeutil.h"

namespace {

struct StatisticsState {
  std::mutex mutex;
  std::map<std::string, double> phases;
  std::map<std::string, size_t> counts;
  std::map<std::string, std::unique_ptr<Stats>> statistics;
};
#define g_mu (SessionService<StatisticsState>().mutex)
#define phase_times (SessionService<StatisticsState>().phases)
#define counters (SessionService<StatisticsState>().counts)

std::string JsonString(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += c;
    } else if (c < 32) {
      result += StringPrintf("\\u%04x", c);
    } else {
      result += c;
    }
  }
  return result + '"';
}
}  // namespace

Stats& SessionStats(const char* name) {
  if (!g_flags.enable_stat_logs && !g_flags.stats_json) {
    struct DisabledStats {
      Stats value{"disabled"};
    };
    return SessionService<DisabledStats>().value;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  auto& slot = SessionService<StatisticsState>().statistics[name];
  if (!slot)
    slot = std::make_unique<Stats>(name);
  return *slot;
}
Stats::Stats(const char* name) : name_(name), elapsed_(0), cnt_(0) {}

void Stats::DumpTop() const {
  std::unique_lock<std::mutex> lock(mu_);
  if (detailed_.size() > 0) {
    std::vector<std::pair<std::string, StatsDetails>> details(detailed_.begin(),
                                                              detailed_.end());
    sort(details.begin(), details.end(),
         [](const std::pair<std::string, StatsDetails> a,
            const std::pair<std::string, StatsDetails> b) -> bool {
           return a.second.elapsed_ > b.second.elapsed_;
         });

    // Only print the top 10
    details.resize(std::min(details.size(), static_cast<size_t>(10)));

    if (!interesting_.empty()) {
      // No need to print anything out twice
      auto interesting = interesting_;
      for (auto& [n, detail] : details) {
        interesting.erase(n);
      }

      for (auto& name : interesting) {
        auto detail = detailed_.find(name);
        if (detail == detailed_.end()) {
          details.emplace_back(name, StatsDetails());
          continue;
        }
        details.emplace_back(*detail);
      }
    }

    int max_cnt_len = 1;
    for (auto& [name, detail] : details) {
      max_cnt_len = std::max(
          max_cnt_len, static_cast<int>(std::to_string(detail.cnt_).length()));
    }

    for (auto& [name, detail] : details) {
      LOG_STAT(" %6.3f / %*d %s", detail.elapsed_, max_cnt_len, detail.cnt_,
               name.c_str());
    }
  }
}

std::string Stats::String() const {
  std::unique_lock<std::mutex> lock(mu_);
  if (!detailed_.empty())
    return StringPrintf("%s: %f / %d (%d unique)", name_, elapsed_, cnt_,
                        detailed_.size());
  return StringPrintf("%s: %f / %d", name_, elapsed_, cnt_);
}

std::string Stats::Json() const {
  std::lock_guard<std::mutex> lock(mu_);
  return "{\"name\":" + JsonString(name_) +
         StringPrintf(",\"count\":%d,\"seconds\":%.9f,\"unique\":%zu}", cnt_,
                      elapsed_, detailed_.size());
}

void RecordPhaseTime(const char* name, double seconds) {
  if (!g_flags.stats_json)
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  phase_times[name] += seconds;
}

void RecordStatCounter(const char* name, size_t count) {
  if (!g_flags.stats_json)
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  counters[name] += count;
}

void RecordStatMaximum(const char* name, size_t value) {
  if (!g_flags.stats_json)
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  counters[name] = std::max(counters[name], value);
}

double Stats::Start() {
  double start = GetMonotonicTime();
  std::unique_lock<std::mutex> lock(mu_);
  cnt_++;
  return start;
}

double Stats::End(double start, const char* msg) {
  double e = GetMonotonicTime() - start;
  std::unique_lock<std::mutex> lock(mu_);
  elapsed_ += e;
  if (msg != 0) {
    StatsDetails& details = detailed_[std::string(msg)];
    details.elapsed_ += e;
    details.cnt_++;
  }
  return e;
}

void Stats::MarkInteresting(const std::string& msg) {
  std::unique_lock<std::mutex> lock(mu_);
  interesting_.emplace(msg);
}

ScopedStatsRecorder::ScopedStatsRecorder(Stats* st, const char* msg)
    : st_(st), msg_(msg), start_time_(0) {
  if (!g_flags.enable_stat_logs)
    return;
  start_time_ = st_->Start();
}

ScopedStatsRecorder::~ScopedStatsRecorder() {
  if (!g_flags.enable_stat_logs)
    return;
  double e = st_->End(start_time_, msg_);
  if (msg_ && e > 3.0) {
    LOG_STAT("slow %s (%f): %s", st_->name_, e, msg_);
  }
}

void ReportAllStats() {
  std::vector<Stats*> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& item : SessionService<StatisticsState>().statistics)
      snapshot.push_back(item.second.get());
  }
  for (Stats* st : snapshot) {
    LOG_STAT("%s", st->String().c_str());
    st->DumpTop();
  }
  LOG_STAT("%u find nodes", FindEmulator::GetNodeCount());
  if (!g_flags.stats_json)
    return;
  struct rusage usage = {};
  getrusage(RUSAGE_SELF, &usage);
  std::string json = StringPrintf(
      "{\"schema\":1,\"pid\":%ld,\"clock\":\"CLOCK_MONOTONIC\","
      "\"peak_rss_kib\":%ld,\"phases\":{",
      static_cast<long>(getpid()), usage.ru_maxrss);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    bool first = true;
    for (const auto& phase : phase_times) {
      if (!first)
        json += ',';
      first = false;
      json += JsonString(phase.first) + StringPrintf(":%.9f", phase.second);
    }
    json += "},\"counters\":{";
    first = true;
    for (const auto& counter : counters) {
      if (!first)
        json += ',';
      first = false;
      json += JsonString(counter.first) + StringPrintf(":%zu", counter.second);
    }
  }
  json += "},\"statistics\":[";
  std::vector<std::string> records;
  for (Stats* stat : snapshot)
    records.push_back(stat->Json());
  std::sort(records.begin(), records.end());
  for (size_t i = 0; i < records.size(); ++i) {
    if (i)
      json += ',';
    json += records[i];
  }
  json += "]}\n";
  const int fd =
      open(g_flags.stats_json, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    WARN("cannot open statistics file %s: %s", g_flags.stats_json,
         strerror(errno));
    return;
  }
  ssize_t written;
  do {
    written = write(fd, json.data(), json.size());
  } while (written < 0 && errno == EINTR);
  if (written < 0 || static_cast<size_t>(written) != json.size())
    WARN("cannot append complete statistics record to %s", g_flags.stats_json);
  close(fd);
}
