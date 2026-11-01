// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef RESOLUTION_CACHE_H_
#define RESOLUTION_CACHE_H_
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "observation.h"

// Persistent pure implicit-chain feasibility results. Graph construction,
// secondary expansion, target scopes and Makefile effects are never replayed.
class ResolutionCache {
 public:
  explicit ResolutionCache(std::string identity, bool enabled = true);
  ~ResolutionCache();
  bool Resolve(const std::string& key, const std::function<bool()>& evaluate);

 private:
  struct Entry {
    bool result;
    std::vector<ObservationRecord> observations;
  };
  std::string identity_, path_;
  std::map<std::string, Entry> entries_;
  size_t bytes_ = 0, work_ = 0;
  bool enabled_ = true, dirty_ = false;
};
#endif
