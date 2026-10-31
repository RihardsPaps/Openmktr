// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "recipe_state.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <charconv>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "fileutil.h"
#include "graph_optimization.h"
#include "log.h"
#include "session.h"
#include "strutil.h"

namespace {
constexpr size_t kPackLimit = 1024 * 1024;
constexpr size_t kRecordLimit = 256;
struct Publication {
  RecipeStateBatch* batch = nullptr;
};
struct Pack {
  explicit Pack(int fd, bool sealed, size_t size)
      : fd(fd), sealed(sealed), size(size) {}
  ~Pack() { close(fd); }
  int fd;
  bool sealed;
  size_t size;
};
struct PackReaders {
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<Pack>> files;
  std::shared_ptr<Pack> Open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = files.find(path);
    if (found != files.end())
      return found->second;
    const int fd =
        open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
      return {};
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > kPackLimit) {
      close(fd);
      return {};
    }
    auto pack =
        std::make_shared<Pack>(fd, (st.st_mode & 07777) == 0400, st.st_size);
    if (files.size() == 64)
      files.erase(files.begin());
    files.emplace(path, pack);
    return pack;
  }
};
bool Number(std::string_view value, size_t* result) {
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), *result);
  return !value.empty() && parsed.ec == std::errc() &&
         parsed.ptr == value.data() + value.size();
}
bool Read(const std::string& path,
          std::string* state,
          std::string* pack_path = nullptr,
          bool* sealed = nullptr) {
  // This private symlink is a descriptor, never a pathname to follow. Keep it
  // short, parse every field, and open only a regular pack in the same folder.
  char buffer[128];
  const ssize_t length = readlink(path.c_str(), buffer, sizeof(buffer));
  if (length <= 0 || static_cast<size_t>(length) == sizeof(buffer))
    return false;
  std::string_view descriptor(buffer, length);
  constexpr std::string_view magic = "omktr-p1:";
  if (!HasPrefix(descriptor, magic))
    return false;
  descriptor.remove_prefix(magic.size());
  const auto colon = descriptor.find(':');
  if (colon == std::string_view::npos)
    return false;
  const auto name = descriptor.substr(0, colon);
  if (!HasPrefix(name, ".pack-") || name.size() == 6 ||
      name.find_first_not_of("0123456789.", 6) != std::string_view::npos)
    return false;
  descriptor.remove_prefix(colon + 1);
  const auto separator = descriptor.find(':');
  size_t offset, size;
  if (separator == std::string_view::npos ||
      !Number(descriptor.substr(0, separator), &offset) ||
      !Number(descriptor.substr(separator + 1), &size) || offset > kPackLimit ||
      size > kPackLimit - offset)
    return false;
  const std::string file = std::string(Dirname(path)) + "/" + std::string(name);
  const auto pack = SessionService<PackReaders>().Open(file);
  if (!pack || offset > pack->size || size > pack->size - offset)
    return false;
  state->resize(size);
  size_t read = 0;
  while (read < size) {
    const ssize_t n = HANDLE_EINTR(
        pread(pack->fd, state->data() + read, size - read, offset + read));
    if (n <= 0)
      return false;
    read += n;
  }
  const size_t marker = path.rfind(".packed-") + 8;
  const auto end = path.find('.', marker);
  if (!HasPrefix(*state, "OMKTR_RECIPE1\n") ||
      GraphDigest(*state) != path.substr(marker, end - marker))
    return false;
  if (pack_path)
    *pack_path = file;
  if (sealed)
    *sealed = pack->sealed;
  return true;
}
}  // namespace

bool IsPackedRecipeState(const std::string& path) {
  const auto marker = path.rfind(".packed-");
  return marker != std::string::npos && marker + 8 < path.size() &&
         path.find_first_not_of("0123456789.", marker + 8) == std::string::npos;
}
bool ReadPackedRecipeState(const std::string& path, std::string* state) {
  return Read(path, state);
}

struct RecipeStateBatch::Impl {
  struct Range {
    size_t offset, size;
  };
  std::string directory, bytes;
  std::unordered_map<std::string, Range> pending;
  void Flush() {
    if (pending.empty())
      return;
    const std::string pack = PublishGraphObject(
        directory + "/.pack-" + GraphDigest(bytes), bytes, true);
    for (const auto& [path, range] : pending) {
      const std::string descriptor = "omktr-p1:" + std::string(Basename(pack)) +
                                     ":" + std::to_string(range.offset) + ":" +
                                     std::to_string(range.size);
      if (symlink(descriptor.c_str(), path.c_str()) != 0) {
        // A competing publisher is acceptable only when all bytes agree.
        std::string existing, existing_pack;
        bool sealed = false;
        if (errno != EEXIST ||
            !Read(path, &existing, &existing_pack, &sealed) ||
            existing !=
                std::string_view(bytes).substr(range.offset, range.size))
          ERROR("cannot publish immutable recipe descriptor: %s", path.c_str());
        SyncPublishedGraphObject(existing_pack, sealed);
      }
    }
    SyncGraphObjectDirectory(directory);
    pending.clear();
    bytes.clear();
  }
  std::string Publish(const std::string& owner, const std::string& state) {
    const std::string parent(Dirname(owner));
    if (parent != directory || pending.size() == kRecordLimit ||
        state.size() > kPackLimit - bytes.size()) {
      Flush();
      directory = parent;
    }
    const std::string base = owner + ".packed-" + GraphDigest(state);
    for (size_t collision = 0; collision < 1024; ++collision) {
      const std::string path =
          base + (collision ? "." + std::to_string(collision) : "");
      const auto found = pending.find(path);
      if (found != pending.end()) {
        if (std::string_view(bytes).substr(found->second.offset,
                                           found->second.size) == state)
          return path;
        continue;
      }
      std::string previous, pack;
      bool sealed = false;
      if (Read(path, &previous, &pack, &sealed) && previous == state) {
        SyncPublishedGraphObject(pack, sealed);
        // A crash may have left a complete descriptor whose directory entry
        // was never synced, even if its previously published pack was sealed.
        SyncGraphObjectDirectory(parent);
        return path;
      }
      char link[1];
      if (readlink(path.c_str(), link, sizeof(link)) >= 0 || errno != ENOENT)
        continue;
      pending.emplace(path, Range{bytes.size(), state.size()});
      bytes += state;
      return path;
    }
    ERROR("cannot publish immutable recipe state: %s", base.c_str());
    return {};
  }
};
RecipeStateBatch::RecipeStateBatch()
    : impl_(std::make_unique<Impl>()),
      previous_(SessionService<Publication>().batch) {
  SessionService<Publication>().batch = this;
}
RecipeStateBatch::~RecipeStateBatch() {
  SessionService<Publication>().batch = previous_;
}
void RecipeStateBatch::Flush() {
  impl_->Flush();
}
std::string PublishNinjaRecipeState(const std::string& owner,
                                    const std::string& state) {
  auto* batch = SessionService<Publication>().batch;
  if (batch && state.size() <= kPackLimit)
    return batch->impl_->Publish(owner, state);
  return PublishGraphObject(owner + ".version-" + GraphDigest(state), state,
                            true);
}
