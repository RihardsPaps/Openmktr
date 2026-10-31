// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "generation.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "fileutil.h"
#include "flags.h"
#include "graph_optimization.h"
#include "log.h"
#include "session.h"

namespace fs = std::filesystem;
namespace {
struct Descriptor {
  explicit Descriptor(int value) : value(value) {}
  ~Descriptor() {
    if (value >= 0)
      close(value);
  }
  int get() const { return value; }
  int value;
};
struct GenerationState {
  std::string directory;
};
std::string OutputDirectory() {
  return g_flags.ninja_dir ? g_flags.ninja_dir : ".";
}
std::string Suffix() {
  return g_flags.ninja_suffix ? g_flags.ninja_suffix : "";
}
std::string Root() {
  return OutputDirectory() + "/.omktr-generations/" + GraphDigest(Suffix());
}
std::vector<std::string> Artifacts() {
  const auto suffix = Suffix();
  return {"build" + suffix + ".ninja",
          "env" + suffix + ".sh",
          "ninja" + suffix + ".sh",
          ".kati_stamp" + suffix,
          ".kati_stamp" + suffix + ".includes",
          ".kati_intermediate_states" + suffix,
          ".kati_contexts-build" + suffix + ".ninja"};
}
int Lock(const std::string& path, int mode) {
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0)
    return -1;
  if (HANDLE_EINTR(flock(fd, mode)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}
void SyncDirectory(const std::string& path) {
  Descriptor fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (fd.get() < 0 || fsync(fd.get()) != 0)
    PERROR("sync generation directory %s", path.c_str());
}
void SyncFile(const std::string& path) {
  Descriptor fd(
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0 || fsync(fd.get()) != 0)
    PERROR("sync generation artifact %s", path.c_str());
}
std::string Resolve(const std::string& path) {
  std::error_code error;
  auto resolved = fs::canonical(path, error);
  return error ? std::string() : resolved.string();
}
void Point(const std::string& path, const std::string& destination) {
  const std::string temporary = path + ".link-" + std::to_string(getpid());
  unlink(temporary.c_str());
  if (symlink(destination.c_str(), temporary.c_str()) != 0)
    PERROR("create generation link %s", path.c_str());
  if (rename(temporary.c_str(), path.c_str()) != 0) {
    const int error = errno;
    unlink(temporary.c_str());
    errno = error;
    PERROR("publish generation link %s", path.c_str());
  }
}
bool Validate(const std::string& directory) {
  std::string manifest;
  if (!ReadGraphCache(directory + "/manifest", &manifest))
    return false;
  std::string_view data(manifest);
  std::string version, name, identity;
  if (!ConsumeGraphString(&data, &version) || version != "generation-v1")
    return false;
  size_t count = 0;
  while (!data.empty()) {
    if (++count > 16 || !ConsumeGraphString(&data, &name) || name.empty() ||
        name.find('/') != std::string::npos ||
        !ConsumeGraphString(&data, &identity) ||
        GetFileIdentity(directory + "/" + name) != identity)
      return false;
  }
  return count >= 3;
}
void Collect(const std::string& root, const std::string& current) {
  // Bounded maintenance. A launched reader holds a shared lease; an abandoned
  // staging directory has no manifest. Retain the preceding bundle for
  // recovery.
  std::vector<fs::directory_entry> entries;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    if (entries.size() == 256)
      break;
    if (entry.is_directory(error) && !entry.is_symlink(error) &&
        entry.path().filename().string().compare(0, 2, "g-") == 0 &&
        entry.path().string() != current)
      entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
    return a.last_write_time(error) > b.last_write_time(error);
  });
  for (size_t i = 1; i < entries.size(); ++i) {
    const auto path = entries[i].path().string();
    int fd = Lock(path + "/.lease", LOCK_EX | LOCK_NB);
    if (fd < 0)
      continue;
    fs::remove_all(path, error);
    close(fd);
  }
}
}  // namespace
std::string ResolveGenerationArtifact(const std::string& path) {
  std::error_code error;
  if (!fs::is_symlink(path, error))
    return path;
  const std::string resolved = Resolve(path);
  if (resolved.empty() ||
      fs::path(resolved).filename() != fs::path(path).filename())
    return path;
  const std::string parent = fs::path(resolved).parent_path().string();
  return Validate(parent) ? resolved : path;
}
std::string GenerationPath(const std::string& directory,
                           const std::string& name) {
  const auto& pinned = SessionService<GenerationState>().directory;
  const auto artifacts = Artifacts();
  const bool bundled =
      std::find(artifacts.begin(), artifacts.end(), name) != artifacts.end() ||
      name == ".kati_stamp" + Suffix() + ".tmp";
  return (bundled && !pinned.empty() ? pinned : directory) + "/" + name;
}
GenerationReader::GenerationReader(const std::string& explicit_directory) {
  auto& state = SessionService<GenerationState>();
  previous_ = state.directory;
  directory_ = explicit_directory.empty() ? Resolve(Root() + "/current")
                                          : Resolve(explicit_directory);
  if (directory_.empty())
    return;
  descriptor_ = Lock(directory_ + "/.lease", LOCK_SH);
  if (descriptor_ < 0 || !Validate(directory_))
    return;
  if (explicit_directory.empty()) {
    // A missing public artifact also requires repair; do not silently accept a
    // valid hidden bundle whose entry points have been removed.
    for (const auto& name : Artifacts()) {
      if (fs::exists(directory_ + "/" + name) &&
          Resolve(OutputDirectory() + "/" + name) != directory_ + "/" + name)
        return;
    }
  }
  state.directory = directory_;
  valid_ = true;
}
int GenerationReader::InheritLease() {
  // Keep the lease in the launcher shell itself. This avoids an additional
  // supervisor process, and its Ninja child closes the duplicated descriptor.
  const int inherited = fcntl(descriptor_, F_DUPFD, 20);
  if (inherited < 0)
    PERROR("inherit generation lease");
  close(descriptor_);
  descriptor_ = inherited;
  return inherited;
}
GenerationReader::~GenerationReader() {
  SessionService<GenerationState>().directory = previous_;
  if (descriptor_ >= 0)
    close(descriptor_);
}
GenerationTransaction::GenerationTransaction() {
  root_ = fs::absolute(Root()).lexically_normal().string();
  fs::create_directories(root_);
  lock_ = Lock(root_ + "/.writer", LOCK_EX);
  if (lock_ < 0)
    PERROR("lock generation writer");
  std::string pattern = root_ + "/g-XXXXXX";
  if (!mkdtemp(pattern.data())) {
    close(lock_);
    lock_ = -1;
    PERROR("create generation");
  }
  directory_ = pattern;
  previous_ = SessionService<GenerationState>().directory;
  SessionService<GenerationState>().directory = directory_;
}
GenerationTransaction::~GenerationTransaction() {
  SessionService<GenerationState>().directory = previous_;
  if (!committed_) {
    std::error_code error;
    fs::remove_all(directory_, error);
  }
  if (lock_ >= 0)
    close(lock_);
}
void GenerationTransaction::SyncObjects() {
  objects_.Flush();
}
void GenerationTransaction::Commit() {
  SyncObjects();
  std::string manifest;
  AppendGraphString("generation-v1", &manifest);
  const auto artifacts = Artifacts();
  for (size_t i = 0; i < artifacts.size(); ++i) {
    const auto path = directory_ + "/" + artifacts[i];
    if (!fs::exists(path)) {
      if (i < 4)
        ERROR("incomplete generation: %s", path.c_str());
      continue;
    }
    SyncFile(path);
    // Stamp's checkpoint is refreshed after validation. Its sealed structural
    // portion is checked by StampChecker, not by a mutable file identity here.
    if (i != 3) {
      AppendGraphString(artifacts[i], &manifest);
      AppendGraphString(GetFileIdentity(path), &manifest);
    }
  }
  if (!WriteGraphCache(directory_ + "/manifest", manifest))
    ERROR("write generation manifest");
  SyncFile(directory_ + "/manifest");
  SyncDirectory(directory_);
  // Migration: install a complete legacy snapshot behind the pointer before
  // replacing any old entry point. A crash midway still resolves old contents.
  if (Resolve(root_ + "/current").empty()) {
    const std::string legacy = root_ + "/legacy";
    fs::create_directories(legacy);
    for (const auto& name : artifacts) {
      const auto source = OutputDirectory() + "/" + name;
      std::error_code error;
      if (fs::is_regular_file(source, error)) {
        const auto target = legacy + "/" + name;
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        SyncFile(target);
      }
    }
    SyncDirectory(legacy);
    Point(root_ + "/current", "legacy");
    SyncDirectory(root_);
    SyncDirectory(fs::path(root_).parent_path().string());
  }
  for (size_t i = 0; i < artifacts.size(); ++i) {
    const auto path = OutputDirectory() + "/" + artifacts[i];
    // Cache paths may be occupied by a user's directory. Keep its contents;
    // that invocation simply cannot reuse the public cache entry next time.
    if (i >= 3 && fs::is_directory(path))
      continue;
    Point(path, root_ + "/current/" + artifacts[i]);
  }
  SyncDirectory(OutputDirectory());
  Point(root_ + "/current", fs::path(directory_).filename().string());
  committed_ = true;
  SyncDirectory(root_);
  Collect(root_, directory_);
}
