// FORK MODIFICATION NOTICE (2026)
// Separate runtime services and preserve scoped resource ownership.
// Preserve owned path strings through filesystem-probe cache lookups.
// Track and validate incremental evaluation, include proofs and imported Ninja
// graphs. Changed by the Openmktr fork, maintained by Rihards Paps and Haralds
// Paps. Changed build execution, evaluation, or portability for this fork.
// Validate recursive reuse, regeneration inputs, and shared recipe budgets.
// Bypass cache bookkeeping in observation regions overlapping active effects.
// Expose validation of inherited FIFO jobservers for recursive scheduling.
// Share signal handling and interrupted-output identity checks across modes.
// Expose process-owned load-admission slots for generated Ninja recipes.
// Allow recipe supervision to clean changed outputs on interruption.
// Preserve exact managed-workspace recipe identities across Ninja generations.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
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

#ifndef FILEUTIL_H_
#define FILEUTIL_H_

#include <errno.h>
#include <signal.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

bool Exists(std::string_view f);
bool Exists(const std::string& f);
bool Exists(const char* f);
double GetTimestampFromStat(const struct stat& st);
double GetTimestamp(std::string_view f);
double GetTimestamp(const std::string& f);
double GetTimestamp(const char* f);
// Identity includes ctime so timestamp-preserving replacements invalidate
// caches.
std::string GetFileIdentity(std::string_view path);
std::string GetFileIdentityFromStat(const struct stat& st);
struct FileObservationState;
// Reused only by serialized recipe expansion. Active commands and filesystem
// effects invalidate it across executor threads through a process-wide epoch.
class FileObservationCache {
 public:
  ~FileObservationCache();

 private:
  std::shared_ptr<FileObservationState> state_;
  size_t effect_bypasses_ = 0;
  friend class ScopedFileObservations;
};

// Resolver-local observations, optionally retained across pure expansions.
class ScopedFileObservations {
 public:
  explicit ScopedFileObservations(bool enabled = true,
                                  FileObservationCache* cache = nullptr);
  ~ScopedFileObservations();
  bool valid() const;
  std::unordered_map<std::string, std::string> Identities() const;
  ScopedFileObservations(const ScopedFileObservations&) = delete;
  ScopedFileObservations& operator=(const ScopedFileObservations&) = delete;

 private:
  std::shared_ptr<FileObservationState> state_;
  FileObservationState* previous_;
  friend bool ObserveFile(const char*, struct stat*);
  friend bool ObserveFile(const std::string&, struct stat*);
  friend void InvalidateFileObservations();
  static thread_local FileObservationState* active_;
};
bool ObserveFile(const char* path, struct stat* st);
bool ObserveFile(const std::string& path, struct stat* st);
void InvalidateFileObservations();
uint64_t FilesystemObservationEpoch();
bool FilesystemEffectsActive();
class ScopedFilesystemEffect {
 public:
  ScopedFilesystemEffect();
  ~ScopedFilesystemEffect();
  ScopedFilesystemEffect(const ScopedFilesystemEffect&) = delete;
  ScopedFilesystemEffect& operator=(const ScopedFilesystemEffect&) = delete;
};
bool ReadNinjaRecipeState(const std::string& path, std::string* state);
void WriteNinjaRecipeState(const std::string& path, const std::string& state);

std::string GetOutputStamp(std::string_view path);
void DeleteChangedOutput(std::string_view path, const std::string& previous);

std::string GetExecutablePath();

using GlobMap = std::unordered_map<std::string, std::vector<std::string>>;

// Expand one make wildcard pattern.  The view need not be NUL terminated;
// callers commonly pass a token from WordScanner whose storage is followed by
// more words in the same string.
const GlobMap::mapped_type& Glob(std::string_view pat);

const GlobMap& GetAllGlobCache();

void ClearGlobCache();
// Forget live query results while retaining the invocation's observations.
void InvalidateGlobCache();
bool HasInconsistentGlobObservations();

#define HANDLE_EINTR(x)                  \
  ({                                     \
    int r;                               \
    do {                                 \
      r = (x);                           \
    } while (r == -1 && errno == EINTR); \
    r;                                   \
  })

#endif  // FILEUTIL_H_
