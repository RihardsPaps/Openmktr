// FORK MODIFICATION NOTICE (2026)
// Reuse the running executable identity within each invocation.
// Own invocation state and publish coherent generations with typed freshness.
// Keep filesystem identities, observation epochs, globbing and recipe state
// separate from process supervision. Reject nonregular recipe-state files
// without blocking, and retain bounded atomic state publication.
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

// +build ignore

#include "fileutil.h"
#include "observation.h"
#include "recipe_state.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>

#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <unordered_map>
#include <unordered_set>

#include "flags.h"
#include "graph_optimization.h"
#include "log.h"
#include "strutil.h"

namespace {
struct FilesystemSession {
  std::atomic<uint64_t> epoch{0};
  std::atomic<size_t> effects{0};
};

}  // namespace

uint64_t FilesystemObservationEpoch() {
  return SessionService<FilesystemSession>().epoch.load(
      std::memory_order_acquire);
}

bool FilesystemEffectsActive() {
  return SessionService<FilesystemSession>().effects.load(
             std::memory_order_acquire) != 0;
}

ScopedFilesystemEffect::ScopedFilesystemEffect() {
  SessionService<FilesystemSession>().effects.fetch_add(
      1, std::memory_order_acq_rel);
  InvalidateFileObservations();
}

ScopedFilesystemEffect::~ScopedFilesystemEffect() {
  InvalidateFileObservations();
  SessionService<FilesystemSession>().effects.fetch_sub(
      1, std::memory_order_release);
}

struct FileObservationState {
  struct Observation {
    struct stat metadata;
    int error;
  };
  std::unordered_map<std::string, Observation> files;
  bool valid = true;
  uint64_t epoch = FilesystemObservationEpoch();
  size_t hits = 0;
  size_t misses = 0;
  size_t fallbacks = 0;
};
thread_local FileObservationState* ScopedFileObservations::active_ = nullptr;

FileObservationCache::~FileObservationCache() {
  if (state_)
    LOG_STAT("recipe metadata probes: %zu hits, %zu misses, %zu fallbacks",
             state_->hits, state_->misses, state_->fallbacks);
  if (effect_bypasses_)
    LOG_STAT("recipe metadata expansion bypasses: %zu", effect_bypasses_);
}

ScopedFileObservations::ScopedFileObservations(bool enabled,
                                               FileObservationCache* cache)
    : previous_(active_) {
  if (!enabled)
    return;
  // An already-running effect makes every probe uncertain. Use ordinary
  // probes for this entire region instead of checking the unusable cache on
  // each lookup. An enclosing observation cannot certify omitted probes.
  if (FilesystemEffectsActive()) {
    if (cache)
      ++cache->effect_bypasses_;
    if (previous_)
      previous_->valid = false;
    active_ = nullptr;
    return;
  }
  if (cache) {
    if (!cache->state_)
      cache->state_ = std::make_shared<FileObservationState>();
    state_ = cache->state_;
  } else {
    state_ = std::make_shared<FileObservationState>();
  }
  active_ = state_.get();
}
ScopedFileObservations::~ScopedFileObservations() {
  active_ = previous_;
}
bool ScopedFileObservations::valid() const {
  return state_ && state_->valid &&
         state_->epoch == FilesystemObservationEpoch() &&
         SessionService<FilesystemSession>().effects.load(
             std::memory_order_acquire) == 0;
}
std::unordered_map<std::string, std::string>
ScopedFileObservations::Identities() const {
  std::unordered_map<std::string, std::string> result;
  if (!state_)
    return result;
  for (const auto& file : state_->files) {
    result[file.first] = file.second.error == 0
                             ? GetFileIdentityFromStat(file.second.metadata)
                         : file.second.error == ENOENT ? "missing"
                                                       : "unreadable";
  }
  return result;
}
bool ObserveFile(const char* path, struct stat* st) {
  if (!ScopedFileObservations::active_ && !ObservationLog::Active())
    return stat(path, st) == 0;
  return ObserveFile(std::string(path), st);
}

bool ObserveFile(const std::string& name, struct stat* st) {
  const bool result = [&]() {
    const char* path = name.c_str();
    auto* state = ScopedFileObservations::active_;
    if (!state)
      return stat(path, st) == 0;
    // Preserve the existing C-path interpretation even for embedded NULs.
    if (name.find('\0') != std::string::npos)
      return ObserveFile(path, st);
    const uint64_t epoch = FilesystemObservationEpoch();
    if (state->epoch != epoch) {
      state->files.clear();
      state->valid = false;
      state->epoch = epoch;
    }
    if (SessionService<FilesystemSession>().effects.load(
            std::memory_order_acquire)) {
      ++state->fallbacks;
      state->valid = false;
      state->files.clear();
      return stat(path, st) == 0;
    }
    auto found = state->files.find(name);
    if (found == state->files.end()) {
      if (state->files.size() >= 100000) {
        state->valid = false;
        return stat(path, st) == 0;
      }
      ++state->misses;
      FileObservationState::Observation observation{};
      observation.error = stat(path, &observation.metadata) == 0 ? 0 : errno;
      found = state->files.emplace(name, observation).first;
    } else {
      ++state->hits;
    }
    if (FilesystemObservationEpoch() != epoch ||
        SessionService<FilesystemSession>().effects.load(
            std::memory_order_acquire)) {
      state->files.clear();
      state->valid = false;
      ++state->fallbacks;
      return stat(path, st) == 0;
    }
    *st = found->second.metadata;
    errno = found->second.error;
    return found->second.error == 0;
  }();
  const int error = errno;
  CaptureFileObservation(name, result, *st, error);
  errno = error;
  return result;
}
void InvalidateFileObservations() {
  SessionService<FilesystemSession>().epoch.fetch_add(
      1, std::memory_order_acq_rel);
  if (auto* state = ScopedFileObservations::active_) {
    state->valid = false;
    state->files.clear();
  }
}

bool Exists(std::string_view filename) {
  CHECK(filename.size() < PATH_MAX);
  return Exists(std::string(filename));
}

bool Exists(const std::string& filename) {
  CHECK(filename.size() < PATH_MAX);
  struct stat st;
  return ObserveFile(filename, &st);
}

bool Exists(const char* filename) {
  CHECK(strlen(filename) < PATH_MAX);
  struct stat st;
  return ObserveFile(filename, &st);
}

double GetTimestampFromStat(const struct stat& st) {
#if defined(__linux__)
  return st.st_mtime + st.st_mtim.tv_nsec * 0.001 * 0.001 * 0.001;
#else
  return st.st_mtime;
#endif
}

double GetTimestamp(std::string_view filename) {
  CHECK(filename.size() < PATH_MAX);
  return GetTimestamp(std::string(filename));
}

double GetTimestamp(const std::string& filename) {
  CHECK(filename.size() < PATH_MAX);
  struct stat st;
  if (!ObserveFile(filename, &st))
    return -2.0;
  return GetTimestampFromStat(st);
}

double GetTimestamp(const char* filename) {
  CHECK(strlen(filename) < PATH_MAX);
  struct stat st;
  if (!ObserveFile(filename, &st)) {
    return -2.0;
  }
  return GetTimestampFromStat(st);
}

std::string GetOutputStamp(std::string_view path) {
  struct stat st;
  if (lstat(std::string(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return "missing";
#if defined(__APPLE__)
  const auto mtime = st.st_mtimespec;
#else
  const auto mtime = st.st_mtim;
#endif
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(mtime.tv_sec) + ":" + std::to_string(mtime.tv_nsec);
}

bool ReadNinjaRecipeState(const std::string& path, std::string* state) {
  state->clear();
  if (IsPackedRecipeState(path))
    return ReadPackedRecipeState(path, state);
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      info.st_size > 256 * 1024 * 1024) {
    close(fd);
    return false;
  }
  FILE* stream = fdopen(fd, "rb");
  if (!stream) {
    close(fd);
    return false;
  }
  char buffer[8192];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
    if (state->size() + count > 256 * 1024 * 1024) {
      fclose(stream);
      return false;
    }
    state->append(buffer, count);
  }
  const bool valid = !ferror(stream) && HasPrefix(*state, "OMKTR_RECIPE1\n");
  fclose(stream);
  return valid;
}

std::string NinjaRecipeStateOwner(const std::string& path) {
  if (IsPackedRecipeState(path))
    return path.substr(0, path.rfind(".packed-"));
  const size_t marker = path.rfind(".version-");
  return marker == std::string::npos ||
                 path.find_first_not_of("0123456789.", marker + 9) !=
                     std::string::npos
             ? path
             : path.substr(0, marker);
}
void WriteNinjaRecipeState(const std::string& path, const std::string& state) {
  std::string previous;
  if (ReadNinjaRecipeState(path, &previous) && previous == state)
    return;
  std::string temporary = path + ".tmp.XXXXXX";
  const int fd = mkstemp(temporary.data());
  if (fd < 0)
    PERROR("create Ninja recipe state failed");
  size_t written = 0;
  while (written < state.size()) {
    const ssize_t count =
        HANDLE_EINTR(write(fd, state.data() + written, state.size() - written));
    if (count <= 0) {
      const int error = errno;
      close(fd);
      unlink(temporary.c_str());
      errno = error;
      PERROR("write Ninja recipe state failed");
      return;
    }
    written += count;
  }
  if (close(fd) != 0 || rename(temporary.c_str(), path.c_str()) != 0) {
    const int error = errno;
    unlink(temporary.c_str());
    errno = error;
    PERROR("publish Ninja recipe state failed");
  }
}

std::string GetFileIdentity(std::string_view path) {
  struct stat st;
  if (stat(std::string(path).c_str(), &st) != 0)
    return errno == ENOENT ? "missing" : "unreadable";
  return GetFileIdentityFromStat(st);
}

std::string GetFileIdentityFromStat(const struct stat& st) {
#if defined(__APPLE__)
  const auto mt = st.st_mtimespec;
  const auto ct = st.st_ctimespec;
#else
  const auto mt = st.st_mtim;
  const auto ct = st.st_ctim;
#endif
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(st.st_mode) + ":" + std::to_string(st.st_size) + ":" +
         std::to_string(mt.tv_sec) + ":" + std::to_string(mt.tv_nsec) + ":" +
         std::to_string(ct.tv_sec) + ":" + std::to_string(ct.tv_nsec);
}

void DeleteChangedOutput(std::string_view path, const std::string& previous) {
  ScopedFilesystemEffect effect;
  const std::string current = GetOutputStamp(path);
  if (current != "missing" && current != previous)
    unlink(std::string(path).c_str());
}

namespace {
std::string ReadExecutablePath() {
#if defined(__linux__)
  char mypath[PATH_MAX + 1];
  ssize_t l = readlink("/proc/self/exe", mypath, PATH_MAX);
  if (l < 0) {
    PERROR("readlink for /proc/self/exe");
  }
  mypath[l] = '\0';
  return {mypath};
#elif defined(__APPLE__)
  char mypath[PATH_MAX + 1];
  uint32_t size = PATH_MAX;
  if (_NSGetExecutablePath(mypath, &size) != 0) {
    ERROR("_NSGetExecutablePath failed");
  }
  mypath[size] = 0;
  return {mypath};
#else
#error "Unsupported OS"
#endif
}
struct ExecutablePath {
  const std::string value = ReadExecutablePath();
};
}  // namespace

std::string GetExecutablePath() {
  return BuildSession::Active() ? SessionService<ExecutablePath>().value
                                : ReadExecutablePath();
}

namespace {

class GlobCache {
 public:
  ~GlobCache() { Clear(); }
  const GlobMap::mapped_type& Get(std::string_view pat) {
    const uint64_t filesystem_epoch = FilesystemObservationEpoch();
    if (filesystem_epoch != filesystem_epoch_ || FilesystemEffectsActive()) {
      Invalidate();
      filesystem_epoch_ = filesystem_epoch;
    }
    // WordScanner returns views into a larger expansion buffer.  Materialize
    // the cache key before calling any C API: pat.data() is not required to
    // point at a NUL-terminated string.
    std::string key(pat);
    auto& cached = live_[key];
    if (cached.epoch != epoch_) {
      GlobMap::mapped_type files;
      if (strcspn(key.c_str(), "?*[\\") != key.size()) {
        glob_t gl = {};
        int result = glob(key.c_str(), 0, NULL, &gl);
        // glob() may fail with GLOB_NOMATCH, GLOB_ABORTED, or GLOB_NOSPACE.
        // Its output is not valid on failure, so never inspect gl_pathc or
        // gl_pathv unless the call succeeded.  A failed wildcard expands to
        // no files, matching the behavior expected by make wildcard users.
        if (result == 0) {
          for (size_t i = 0; i < gl.gl_pathc; i++)
            files.push_back(gl.gl_pathv[i]);
        }
        globfree(&gl);
      } else {
        if (Exists(key))
          files.push_back(key);
      }
      auto [observation, first] =
          observations_.try_emplace(key, std::move(files));
      if (first) {
        cached.files = &observation->second;
      } else {
        if (observation->second != files)
          inconsistent_ = true;
        if (!cached.files || *cached.files != files) {
          if (observation->second == files) {
            cached.files = &observation->second;
          } else {
            changed_.emplace_back(std::move(files));
            cached.files = &changed_.back();
          }
        }
      }
      cached.epoch = epoch_;
    }
    if (filesystem_epoch != FilesystemObservationEpoch() ||
        FilesystemEffectsActive())
      inconsistent_ = true;
    return *cached.files;
  }

  const GlobMap& GetAll() const { return observations_; }
  bool inconsistent() const { return inconsistent_; }

  void Invalidate() {
    if (++epoch_ == 0) {
      epoch_ = 1;
      for (auto& entry : live_)
        entry.second.epoch = 0;
    }
  }

  void Clear() {
    live_.clear();
    observations_.clear();
    changed_.clear();
    epoch_ = 1;
    inconsistent_ = false;
  }

 private:
  struct Result {
    size_t epoch = 0;
    const GlobMap::mapped_type* files = nullptr;
  };
  std::unordered_map<std::string, Result> live_;
  GlobMap observations_;
  // Includes may still be iterating an earlier snapshot when an effect runs.
  std::deque<GlobMap::mapped_type> changed_;
  size_t epoch_ = 1;
  bool inconsistent_ = false;
  uint64_t filesystem_epoch_ = 0;
};

#define g_gc (SessionService<GlobCache>())

}  // namespace

const GlobMap::mapped_type& Glob(std::string_view pat) {
  const auto& result = g_gc.Get(pat);
  if (ObservationLog::Active()) {
    ObservationRecord record{ObservationKind::Glob, {std::string(pat)}};
    record.fields.insert(record.fields.end(), result.begin(), result.end());
    ObservationLog::Record(std::move(record));
  }
  return result;
}

const GlobMap& GetAllGlobCache() {
  return g_gc.GetAll();
}

void ClearGlobCache() {
  g_gc.Clear();
}

void InvalidateGlobCache() {
  g_gc.Invalidate();
}

bool HasInconsistentGlobObservations() {
  return g_gc.inconsistent();
}
