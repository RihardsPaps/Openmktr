// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "graph_optimization.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fileutil.h"
#include "jobserver.h"
#include "log.h"
#include "process.h"
#include "session.h"

namespace {
constexpr size_t kMaxCacheSize = 64 * 1024 * 1024;
struct ObjectPublication {
  GraphObjectBatch* batch = nullptr;
};
void SyncObjectPath(const std::string& path, bool directory) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                                  (directory ? O_DIRECTORY : 0));
  struct stat metadata;
  const bool synced =
      fd >= 0 && fstat(fd, &metadata) == 0 &&
      (directory ? S_ISDIR(metadata.st_mode) : S_ISREG(metadata.st_mode)) &&
      fsync(fd) == 0;
  if (fd >= 0)
    close(fd);
  if (!synced)
    ERROR("cannot sync immutable graph object: %s", path.c_str());
}
std::string ObjectParent(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash ? slash : 1);
}
void SealObject(const std::string& path) {
  // Seal only after both bytes and the directory entry are durable. Losing
  // this permission update after a crash merely repeats the next barrier.
  // Objects are private immutable cache files; full bytes are still checked
  // on every reuse. Mutable success/read history never uses this convention.
  if (chmod(path.c_str(), 0400) != 0)
    ERROR("cannot seal immutable graph object: %s", path.c_str());
}

bool StampChecksum(FILE* stream, size_t length, uint64_t* checksum) {
  if (fseek(stream, sizeof(double), SEEK_SET) != 0)
    return false;
  uint64_t hash = UINT64_C(14695981039346656037);
  char buffer[65536];
  while (length) {
    const size_t size = std::min(length, sizeof(buffer));
    if (fread(buffer, 1, size, stream) != size)
      return false;
    for (size_t i = 0; i < size; ++i) {
      hash ^= static_cast<unsigned char>(buffer[i]);
      hash *= UINT64_C(1099511628211);
    }
    length -= size;
  }
  *checksum = hash;
  return true;
}

bool WriteAll(int fd, std::string_view data) {
  while (!data.empty()) {
    ssize_t n = HANDLE_EINTR(write(fd, data.data(), data.size()));
    if (n <= 0)
      return false;
    data.remove_prefix(n);
  }
  return true;
}

// -1: comparison failed, 0: different bytes, 1: identical bytes.
int SameBytes(int first, int second) {
  char a[65536], b[65536];
  if (lseek(first, 0, SEEK_SET) < 0)
    return -1;
  for (;;) {
    ssize_t n = HANDLE_EINTR(read(first, a, sizeof(a)));
    if (n < 0)
      return -1;
    size_t offset = 0;
    while (offset < static_cast<size_t>(n)) {
      ssize_t m = HANDLE_EINTR(read(second, b + offset, n - offset));
      if (m < 0)
        return -1;
      if (m == 0)
        return 0;
      offset += m;
    }
    if (memcmp(a, b, n) != 0)
      return 0;
    if (n == 0) {
      const ssize_t remaining = HANDLE_EINTR(read(second, b, 1));
      return remaining < 0 ? -1 : remaining == 0 ? 1 : 0;
    }
  }
}
}  // namespace

std::string GraphDigest(std::string_view value) {
  // This checksum detects damaged local cache records. It is not an identity
  // proof: users of hash-selected entries also compare the complete key.
  uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char c : value) {
    hash ^= c;
    hash *= UINT64_C(1099511628211);
  }
  return std::to_string(hash);
}

void AppendGraphString(std::string_view value, std::string* data) {
  const uint64_t length = value.size();
  for (int shift = 0; shift < 64; shift += 8)
    data->push_back(static_cast<char>(length >> shift));
  data->append(value);
}

bool ConsumeGraphString(std::string_view* data, std::string* value) {
  std::string_view view;
  if (!ConsumeGraphStringView(data, &view))
    return false;
  value->assign(view);
  return true;
}

bool ConsumeGraphStringView(std::string_view* data, std::string_view* value) {
  if (data->size() < 8)
    return false;
  uint64_t length = 0;
  for (int i = 0; i < 8; ++i)
    length |= static_cast<uint64_t>(static_cast<unsigned char>((*data)[i]))
              << (8 * i);
  data->remove_prefix(8);
  if (length > data->size() || length > kMaxCacheSize)
    return false;
  *value = data->substr(0, length);
  data->remove_prefix(length);
  return true;
}

bool ReadGraphCache(const std::string& path,
                    std::string* payload,
                    bool* sealed) {
  if (sealed)
    *sealed = false;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat st;
  bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
               static_cast<uint64_t>(st.st_size) <= kMaxCacheSize;
  size_t remaining = valid ? static_cast<size_t>(st.st_size) : 0;
  auto read_all = [&](char* data, size_t length) {
    if (length > remaining)
      return false;
    remaining -= length;
    while (length) {
      ssize_t n = HANDLE_EINTR(read(fd, data, length));
      if (n <= 0)
        return false;
      data += n;
      length -= n;
    }
    return true;
  };
  auto read_string = [&](std::string* value, size_t limit) {
    unsigned char header[8];
    if (!read_all(reinterpret_cast<char*>(header), sizeof(header)))
      return false;
    uint64_t length = 0;
    for (size_t i = 0; i < sizeof(header); ++i)
      length |= static_cast<uint64_t>(header[i]) << (8 * i);
    if (length > remaining || length > limit)
      return false;
    value->resize(length);
    return read_all(value->data(), value->size());
  };
  std::string magic, checksum;
  valid = valid && read_string(&magic, 12) && magic == "OMKTR_GRAPH1" &&
          read_string(&checksum, 64) && read_string(payload, kMaxCacheSize) &&
          remaining == 0 && checksum == GraphDigest(*payload);
  close(fd);
  if (valid && sealed)
    *sealed = (st.st_mode & 07777) == 0400;
  return valid;
}

bool SealGraphStamp(FILE* stream) {
  struct stat metadata;
  if (fflush(stream) != 0 || fstat(fileno(stream), &metadata) != 0 ||
      !S_ISREG(metadata.st_mode) ||
      metadata.st_size < static_cast<off_t>(sizeof(double) + 4) ||
      static_cast<uint64_t>(metadata.st_size) > kMaxCacheSize - 8)
    return false;
  uint64_t checksum;
  if (!StampChecksum(stream, metadata.st_size - sizeof(double), &checksum) ||
      fseek(stream, 0, SEEK_END) != 0)
    return false;
  unsigned char bytes[8];
  for (size_t i = 0; i < sizeof(bytes); ++i)
    bytes[i] = checksum >> (i * 8);
  return fwrite(bytes, 1, sizeof(bytes), stream) == sizeof(bytes) &&
         fflush(stream) == 0;
}

FILE* OpenGraphStamp(const std::string& path, bool writable) {
  int fd = open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC |
                                  O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return nullptr;
  struct stat metadata;
  if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < static_cast<off_t>(sizeof(double) + 4 + 8) ||
      static_cast<uint64_t>(metadata.st_size) > kMaxCacheSize) {
    close(fd);
    errno = EINVAL;
    return nullptr;
  }
  FILE* stream = fdopen(fd, writable ? "rb+" : "rb");
  if (!stream) {
    close(fd);
    return nullptr;
  }
  uint64_t checksum = 0, expected = 0;
  unsigned char bytes[8];
  double checkpoint;
  bool valid =
      fread(&checkpoint, sizeof(checkpoint), 1, stream) == 1 &&
      std::isfinite(checkpoint) && checkpoint > 0 &&
      StampChecksum(stream, metadata.st_size - sizeof(double) - 8, &checksum) &&
      fread(bytes, 1, sizeof(bytes), stream) == sizeof(bytes);
  if (valid)
    for (size_t i = 0; i < sizeof(bytes); ++i)
      expected |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  if (!valid || checksum != expected || fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    errno = EBADMSG;
    return nullptr;
  }
  return stream;
}

bool WriteGraphCache(const std::string& path, std::string_view payload) {
  std::string bytes;
  AppendGraphString("OMKTR_GRAPH1", &bytes);
  AppendGraphString(GraphDigest(payload), &bytes);
  AppendGraphString(payload, &bytes);
  if (bytes.size() > kMaxCacheSize)
    return false;
  std::string temporary = path + ".tmp.XXXXXX";
  int fd = mkstemp(temporary.data());
  if (fd < 0)
    return false;
  bool ok = WriteAll(fd, bytes);
  if (close(fd) != 0)
    ok = false;
  if (ok)
    ok = rename(temporary.c_str(), path.c_str()) == 0;
  unlink(temporary.c_str());
  return ok;
}

GraphObjectBatch::GraphObjectBatch()
    : previous_(SessionService<ObjectPublication>().batch) {
  SessionService<ObjectPublication>().batch = this;
}
GraphObjectBatch::~GraphObjectBatch() {
  SessionService<ObjectPublication>().batch = previous_;
}
void GraphObjectBatch::Add(const std::string& path) {
  paths_.insert(path);
  if (paths_.size() >= 256)
    Flush();
}
void GraphObjectBatch::AddDirectory(const std::string& path) {
  directories_.insert(path);
  if (directories_.size() >= 256)
    Flush();
}
void GraphObjectBatch::Flush() {
  // All writes precede all barriers, letting the filesystem coalesce them.
  // Unsealed existing objects are included: another writer may just have
  // linked one, or abandoned a batch before making its objects durable.
  for (const auto& path : paths_) {
    SyncObjectPath(path, false);
    directories_.insert(ObjectParent(path));
  }
  for (const auto& path : directories_)
    SyncObjectPath(path, true);
  for (const auto& path : paths_)
    SealObject(path);
  paths_.clear();
  directories_.clear();
}

void SyncPublishedGraphObject(const std::string& path, bool sealed) {
  if (sealed)
    return;
  if (auto* batch = SessionService<ObjectPublication>().batch) {
    batch->Add(path);
  } else {
    SyncObjectPath(path, false);
    SyncObjectPath(ObjectParent(path), true);
    SealObject(path);
  }
}
void SyncGraphObjectDirectory(const std::string& path) {
  if (auto* batch = SessionService<ObjectPublication>().batch)
    batch->AddDirectory(path);
  else
    SyncObjectPath(path, true);
}

std::string PublishGraphObject(const std::string& base,
                               std::string_view payload,
                               bool raw) {
  std::string temporary;
  auto finish = [&](const std::string& path, bool sealed = false) {
    SyncPublishedGraphObject(path, sealed);
    return path;
  };
  bool sealed = false;
  auto read = [&](const std::string& path, std::string* bytes) {
    sealed = false;
    if (!raw)
      return ReadGraphCache(path, bytes, &sealed);
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
      return false;
    struct stat metadata;
    bool ok = fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
              metadata.st_size >= 0 &&
              metadata.st_size <= static_cast<off_t>(kMaxCacheSize);
    if (ok) {
      bytes->resize(metadata.st_size);
      size_t offset = 0;
      while (offset < bytes->size()) {
        ssize_t count = HANDLE_EINTR(
            ::read(fd, bytes->data() + offset, bytes->size() - offset));
        if (count <= 0) {
          ok = false;
          break;
        }
        offset += count;
      }
    }
    close(fd);
    if (ok)
      sealed = (metadata.st_mode & 07777) == 0400;
    return ok;
  };
  if (payload.size() > kMaxCacheSize)
    ERROR("immutable graph object exceeds size budget");
  for (size_t collision = 0; collision < 1024; ++collision) {
    const std::string path =
        base + (collision ? "." + std::to_string(collision) : "");
    std::string previous;
    if (read(path, &previous) && previous == payload) {
      if (!temporary.empty())
        unlink(temporary.c_str());
      return finish(path, sealed);
    }
    struct stat metadata;
    if (lstat(path.c_str(), &metadata) == 0)
      continue;
    if (errno != ENOENT)
      break;
    if (temporary.empty()) {
      temporary = base + ".object-XXXXXX";
      int fd = mkstemp(temporary.data());
      if (fd < 0)
        break;
      bool written = !raw || WriteAll(fd, payload);
      if (close(fd) != 0)
        written = false;
      if (!written || (!raw && !WriteGraphCache(temporary, payload)))
        break;
    }
    if (link(temporary.c_str(), path.c_str()) == 0) {
      unlink(temporary.c_str());
      return finish(path);
    }
    if (errno != EEXIST)
      break;
    // A concurrent publisher may have installed these exact bytes.
    if (read(path, &previous) && previous == payload) {
      unlink(temporary.c_str());
      return finish(path, sealed);
    }
  }
  if (!temporary.empty())
    unlink(temporary.c_str());
  ERROR("cannot publish immutable graph object: %s", base.c_str());
  return {};
}

int WriteIfChanged(int argc, char** argv) {
  if (argc < 3 || strcmp(argv[1], "--") != 0) {
    fprintf(stderr,
            "usage: omktr write-if-changed OUTPUT -- COMMAND [ARG...]\n");
    return 2;
  }
  const std::string output = argv[0];
  struct stat original;
  bool existed = lstat(output.c_str(), &original) == 0;
  if ((!existed && errno != ENOENT) ||
      (existed && !S_ISREG(original.st_mode))) {
    fprintf(stderr, "write-if-changed: output must be a regular file: %s\n",
            output.c_str());
    return 2;
  }
  std::string temporary = output + ".tmp.XXXXXX";
  int fd = mkstemp(temporary.data());
  if (fd < 0) {
    perror("write-if-changed: create temporary output");
    return 1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  const std::string original_identity = GetFileIdentity(output);
  mode_t mask = umask(0);
  umask(mask);
  // Keep unpublished bytes private. Set the final mode after producer writes,
  // which may otherwise clear set-user-ID and set-group-ID bits.
  const mode_t mode = existed ? original.st_mode & 07777 : 0666 & ~mask;
  ScopedBuildSignals signals;
  // A helper can be reached through an opaque recursive recipe, whose caller
  // lets its children acquire the shared slots. Treat the producer as one job;
  // an ordinary caller's inherited reservation avoids taking a second slot.
  const int job_token = AcquireKatiJobToken();
  if (const int interrupted = GetBuildSignal()) {
    close(fd);
    unlink(temporary.c_str());
    ReleaseKatiJobToken(job_token);
    return 128 + interrupted;
  }
  const char* fifo = getenv("KATI_JOBSERVER_FIFO");
  const bool forward_slot = (fifo != nullptr && *fifo != '\0') ||
                            getenv("KATI_JOBSERVER_RESERVED") != nullptr;
  pid_t child = fork();
  if (child == 0) {
    if (setpgid(0, 0) < 0)
      _exit(126);
    if (forward_slot &&
        setenv("KATI_JOBSERVER_RESERVED", job_token == -1 ? "0" : "1", 1) < 0)
      _exit(126);
    if (dup2(fd, STDOUT_FILENO) < 0)
      _exit(126);
    if (fd != STDOUT_FILENO)
      close(fd);
    else
      fcntl(fd, F_SETFD, 0);
    execvp(argv[2], argv + 2);
    perror("write-if-changed: execute producer");
    _exit(127);
  }
  int status = 0;
  if (child < 0) {
    status = 1;
  } else {
    setpgid(child, child);
    for (;;) {
      if (GetBuildSignal())
        kill(-child, GetBuildSignal());
      pid_t done = waitpid(child, &status, 0);
      if (done == child)
        break;
      if (done < 0 && errno != EINTR) {
        status = 1 << 8;
        break;
      }
    }
    status = GetBuildSignal()    ? 128 + GetBuildSignal()
             : WIFEXITED(status) ? WEXITSTATUS(status)
                                 : 128 + WTERMSIG(status);
  }
  if (status == 0) {
    if (fchmod(fd, mode) != 0) {
      perror("write-if-changed: preserve output mode");
      status = 1;
    } else {
      struct stat final;
      if (fstat(fd, &final) != 0 || (final.st_mode & 07777) != mode) {
        fprintf(stderr, "write-if-changed: cannot preserve output mode\n");
        status = 1;
      }
    }
  }
  if (status == 0) {
    // Refuse to replace a pathname changed by the producer or another writer.
    struct stat current;
    bool now_exists = lstat(output.c_str(), &current) == 0;
    if (GetFileIdentity(output) != original_identity || existed != now_exists ||
        (now_exists &&
         (!S_ISREG(current.st_mode) || current.st_dev != original.st_dev ||
          current.st_ino != original.st_ino))) {
      status = 1;
    } else {
      int previous = existed ? open(output.c_str(), O_RDONLY | O_NOFOLLOW) : -1;
      const int comparison =
          existed ? (previous < 0 ? -1 : SameBytes(fd, previous)) : 0;
      if (previous >= 0)
        close(previous);
      if (comparison < 0) {
        fprintf(stderr, "write-if-changed: cannot compare existing output\n");
        status = 1;
      } else if (comparison == 0 &&
                 rename(temporary.c_str(), output.c_str()) != 0) {
        perror("write-if-changed: publish output");
        status = 1;
      }
    }
  }
  close(fd);
  unlink(temporary.c_str());
  ReleaseKatiJobToken(job_token);
  return status;
}
