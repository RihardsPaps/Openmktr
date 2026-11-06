// FORK MODIFICATION NOTICE (2026)
// Keep include-producer freshness separate from parsed-source content reuse.
// Track fixdep record contents independently of compiler rewrite timestamps.
// Classify leading-dot aliases consistently without changing Makefile names.
// Own invocation state and publish coherent generations with typed freshness.
// Validate caller environments before reusing deferred recipes or child
// imports. Track and validate incremental evaluation, include proofs and
// imported Ninja graphs. Changed by the Openmktr fork, maintained by Rihards
// Paps and Haralds Paps. Changed build execution, evaluation, or portability
// for this fork. Validate recursive reuse, regeneration inputs, and shared
// recipe budgets. Upstream material retains its Apache-2.0 terms. Fork
// modifications are covered by Mozilla Public License 2.0; see
// docs/LICENSING.md and NOTICE at the repository root. Original notices below
// remain applicable.

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
  void AddContentFileDep(const std::string& path) {
    extra_file_deps_.insert(path);
    content_file_deps_.insert(ObservationPath(path));
  }
  bool IsContentFileDep(const std::string& path) const {
    return content_file_deps_.count(ObservationPath(path)) != 0;
  }
  void AddIncludeDirectoryDep(std::string_view directory) {
    extra_file_deps_.emplace(directory);
    include_directory_deps_.emplace(directory);
  }
  void AddGeneratedGraphDep(const std::string& dep) {
    extra_file_deps_.insert(dep);
    generated_graph_deps_.insert(dep);
  }
  bool IsGeneratedGraphDep(const std::string& dep) const {
    return generated_graph_deps_.count(dep) != 0;
  }
  bool IsParsedMakefile(const std::string& path) const {
    return parsed_paths_.count(ObservationPath(path)) != 0;
  }
  bool IsIncludeDirectoryDep(const std::string& path) const {
    return include_directory_deps_.count(path) != 0;
  }
  void AddIncludeFreshnessDep(const std::string& path) {
    extra_file_deps_.insert(path);
    include_freshness_deps_.insert(ObservationPath(path));
  }
  bool IsIncludeFreshnessDep(const std::string& path) const {
    return include_freshness_deps_.count(ObservationPath(path)) != 0;
  }
  void RequireEvaluation() { require_evaluation_ = true; }
  void RequireEnvironmentValidation() {
    require_environment_validation_ = true;
  }
  bool requires_environment_validation() const {
    return require_environment_validation_;
  }
  void RequireIncludeEvaluation() { require_include_evaluation_ = true; }
  void CertifyIncludeEvaluation() {
    require_include_evaluation_ = false;
    includes_certified_ = true;
  }
  bool includes_certified() const { return includes_certified_; }
  bool requires_evaluation() const {
    return require_evaluation_ || require_include_evaluation_;
  }
  bool has_observable_evaluation() const { return require_evaluation_; }

  static MakefileCacheManager& Get();

  MakefileCacheManager() = default;

 private:
  static std::string ObservationPath(std::string_view path) {
    // Leading ./ components always name the same object. Preserve all other
    // spellings, including .. and symlink components, and keep cache keys and
    // MAKEFILE_LIST unchanged.
    while (path.size() > 2 && path.substr(0, 2) == "./")
      path.remove_prefix(2);
    return std::string(path);
  }
  MakefileCacheManager(const MakefileCacheManager&) = delete;
  MakefileCacheManager(MakefileCacheManager&&) = delete;
  std::unordered_map<std::string, Makefile> cache_;
  std::unordered_set<std::string> parsed_paths_;
  std::unique_ptr<Makefile> stdin_makefile_;
  std::unordered_set<std::string> extra_file_deps_;
  std::unordered_set<std::string> content_file_deps_;
  std::unordered_set<std::string> generated_graph_deps_;
  std::unordered_set<std::string> include_directory_deps_;
  std::unordered_set<std::string> include_freshness_deps_;
  bool require_evaluation_ = false;
  bool require_environment_validation_ = false;
  bool require_include_evaluation_ = false;
  bool includes_certified_ = false;
};

#endif  // FILE_CACHE_H_
