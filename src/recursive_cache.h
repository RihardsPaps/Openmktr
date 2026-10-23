// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef RECURSIVE_CACHE_H_
#define RECURSIVE_CACHE_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dep.h"

class Evaluator;

// A bounded first cache: reuse only a proven no-op graph. Dirty or unsupported
// graphs always take the ordinary evaluator and executor, never a new
// scheduler.
class RecursiveCache {
 public:
  explicit RecursiveCache(const std::string& arguments);
  ~RecursiveCache();
  bool Reuse();
  void Save(const std::vector<NamedDepNode>& roots, Evaluator* ev);
  RecursiveCache(const RecursiveCache&) = delete;
  RecursiveCache& operator=(const RecursiveCache&) = delete;

 private:
  int lock_ = -1;
  std::string key_;
  // Views remain owned by certificate_ through Save. They certify eligibility
  // only: returning a no-op still requires every input/graph identity to match.
  std::string certificate_;
  std::unordered_map<std::string, std::string_view> eligible_sources_;
};

#endif
