// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "resolution_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "graph_optimization.h"
#include "stats.h"
#include "symtab.h"

namespace {
constexpr size_t kStorageBudget = 16 * 1024 * 1024;
constexpr size_t kWorkBudget = 64 * 1024 * 1024;
}  // namespace
ResolutionCache::ResolutionCache(std::string identity, bool enabled)
    : identity_(std::move(identity)), enabled_(enabled) {
  if (!enabled_)
    return;
  identity_ = "resolution-v1" + identity_;
  AppendGraphString(GetFileIdentity(GetExecutablePath()), &identity_);
  path_ = std::string(g_flags.ninja_dir ? g_flags.ninja_dir : ".") +
          "/.kati_resolution" +
          (g_flags.ninja_suffix ? g_flags.ninja_suffix : "");
  if (identity_.size() > kStorageBudget / 4) {
    enabled_ = false;
    return;
  }
  struct stat metadata;
  if (stat(path_.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size > static_cast<off_t>(kStorageBudget))
    return;
  std::string payload, identity_record;
  if (!ReadGraphCache(path_, &payload))
    return;
  std::string_view data(payload);
  if (!ConsumeGraphString(&data, &identity_record) ||
      identity_record != identity_)
    return;
  std::map<std::string, Entry> decoded;
  while (!data.empty()) {
    std::string key, result, observations;
    if (decoded.size() >= 8192 || !ConsumeGraphString(&data, &key) ||
        key.size() > 1024 * 1024 || !ConsumeGraphString(&data, &result) ||
        (result != "0" && result != "1") ||
        !ConsumeGraphString(&data, &observations) ||
        observations.size() > 1024 * 1024)
      return;
    Entry entry{result == "1", {}};
    std::string_view records(observations);
    while (!records.empty()) {
      std::string bytes;
      ObservationRecord record;
      if (entry.observations.size() >= 8192 ||
          !ConsumeGraphString(&records, &bytes) ||
          !ObservationRecord::Decode(bytes, &record) ||
          (record.kind != ObservationKind::Metadata &&
           record.kind != ObservationKind::Glob &&
           record.kind != ObservationKind::Intern))
        return;
      entry.observations.push_back(std::move(record));
    }
    if (!decoded.emplace(std::move(key), std::move(entry)).second)
      return;
  }
  bytes_ = payload.size();
  entries_ = std::move(decoded);
}
ResolutionCache::~ResolutionCache() {
  if (!enabled_ || !dirty_)
    return;
  std::string payload;
  AppendGraphString(identity_, &payload);
  for (const auto& item : entries_) {
    std::string observations;
    for (const auto& record : item.second.observations)
      AppendGraphString(record.Encode(), &observations);
    AppendGraphString(item.first, &payload);
    AppendGraphString(item.second.result ? "1" : "0", &payload);
    AppendGraphString(observations, &payload);
    if (payload.size() > kStorageBudget)
      return;
  }
  // Failed/obsolete/corrupt cache publication is an ordinary cold fallback.
  WriteGraphCache(path_, payload);
}
bool ResolutionCache::Resolve(const std::string& key,
                              const std::function<bool()>& evaluate) {
  if (!enabled_ || work_ > kWorkBudget || FilesystemEffectsActive() ||
      key.size() > 1024 * 1024)
    return evaluate();
  auto found = entries_.find(key);
  if (found != entries_.end()) {
    const auto epoch = FilesystemObservationEpoch();
    bool valid = true;
    for (const auto& observation : found->second.observations) {
      work_ += observation.Encode().size();
      if (work_ > kWorkBudget || !observation.Validate()) {
        valid = false;
        break;
      }
    }
    if (valid && epoch == FilesystemObservationEpoch() &&
        !FilesystemEffectsActive()) {
      // Interning is an observable ordered effect (.KATI_SYMBOLS). Replay it
      // only after every positive and negative filesystem witness validates.
      for (const auto& observation : found->second.observations)
        if (observation.kind == ObservationKind::Intern)
          Intern(observation.fields[0]);
      RecordStatCounter("resolution_hits");
      return found->second.result;
    }
  }
  RecordStatCounter("resolution_misses");
  ObservationLog observations;
  const bool result = evaluate();
  if (observations.valid() && entries_.size() < 8192) {
    size_t bytes = key.size() + 64;
    for (const auto& record : observations.records())
      bytes += record.Encode().size() + 8;
    work_ += bytes;
    if (bytes_ + bytes < kStorageBudget && work_ <= kWorkBudget) {
      entries_[key] = {result, observations.records()};
      bytes_ += bytes;
      dirty_ = true;
    }
  }
  return result;
}
