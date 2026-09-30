// FORK MODIFICATION NOTICE (2026)
// Changed by the GNU-free Kati fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
// at the repository root. Original notices below remain applicable.

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

#ifndef FILE_CACHE_H_
#define FILE_CACHE_H_

#include <memory>
#include <string>
#include <unordered_set>

#include "file.h"

class MakefileCacheManager {
 public:
  const Makefile& ReadMakefile(const std::string& filename);
  const Makefile& ReadStdinMakefile();
  void GetAllFilenames(std::unordered_set<std::string>* out);
  void AddExtraFileDep(std::string_view dep);

  static MakefileCacheManager& Get();

 private:
  MakefileCacheManager() = default;
  MakefileCacheManager(const MakefileCacheManager&) = delete;
  MakefileCacheManager(MakefileCacheManager&&) = delete;
  std::unordered_map<std::string, Makefile> cache_;
  std::unique_ptr<Makefile> stdin_makefile_;
  std::unordered_set<std::string> extra_file_deps_;
};

#endif  // FILE_CACHE_H_
