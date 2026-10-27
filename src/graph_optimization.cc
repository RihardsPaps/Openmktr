// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "graph_optimization.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fileutil.h"

namespace {
constexpr size_t kMaxCacheSize = 64 * 1024 * 1024;

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
  if (data->size() < 8)
    return false;
  uint64_t length = 0;
  for (int i = 0; i < 8; ++i)
    length |= static_cast<uint64_t>(static_cast<unsigned char>((*data)[i]))
              << (8 * i);
  data->remove_prefix(8);
  if (length > data->size() || length > kMaxCacheSize)
    return false;
  value->assign(data->substr(0, length));
  data->remove_prefix(length);
  return true;
}

bool ReadGraphCache(const std::string& path, std::string* payload) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return false;
  struct stat st;
  bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
               static_cast<uint64_t>(st.st_size) <= kMaxCacheSize;
  std::string bytes;
  if (valid) {
    bytes.resize(st.st_size);
    size_t offset = 0;
    while (offset < bytes.size()) {
      ssize_t n = HANDLE_EINTR(read(fd, &bytes[offset], bytes.size() - offset));
      if (n <= 0) {
        valid = false;
        break;
      }
      offset += n;
    }
  }
  close(fd);
  std::string_view view(bytes);
  std::string magic, checksum;
  return valid && ConsumeGraphString(&view, &magic) &&
         magic == "OMKTR_GRAPH1" && ConsumeGraphString(&view, &checksum) &&
         ConsumeGraphString(&view, payload) && view.empty() &&
         checksum == GraphDigest(*payload);
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
  pid_t child = fork();
  if (child == 0) {
    if (setpgid(0, 0) < 0)
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
  return status;
}
